/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Osvaldo Janeri Filho <ojanerif@amd.com>
 *
 * [TC-TSC-DRF] AMD TSC cross-CPU drift tests.
 *
 * Validates that the TSC is synchronised across CPUs (InvariantTSC guarantee).
 * On AMD Zen 1+, the hardware synchronises the TSC across all cores and dies
 * at boot.  Cross-CPU delta should be sub-microsecond on bare metal.
 *
 * 5 test cases:
 *
 * TC-TSC-DRF-01  tsc_drift_same_cpu_zero
 *   Two consecutive tsc_read() calls on the same pinned CPU.  Asserts no
 *   backwards jump and delta < 10 000 cycles (LFENCE overhead bound).
 *   CRITICAL — foundational single-CPU contract.
 *
 * TC-TSC-DRF-02  tsc_drift_two_cpu_monotonic
 *   Fires two threads simultaneously on CPU 0 and CPU(ncpus-1); compares
 *   their TSC reads.  Asserts |delta| < 1 ms worth of cycles.
 *   HIGH — cross-CCX / cross-die synchronisation.
 *
 * TC-TSC-DRF-03  tsc_drift_bound_1000_iterations
 *   1 000 paired TSC reads across two CPUs after sched_yield().  Tracks
 *   max observed delta.  Asserts max < 10 ms worth of cycles.
 *   HIGH — statistical drift bound.
 *
 * TC-TSC-DRF-04  tsc_drift_across_sleep
 *   Reads TSC around a 50 ms usleep().  Asserts the implied frequency
 *   agrees with CPUID within 5%.  Validates TSC does not pause in C-states.
 *   HIGH — C-state invariance.
 *
 * TC-TSC-DRF-05  tsc_drift_amd_invariant_guarantee
 *   Documents the AMD InvariantTSC CPUID guarantee formally.  Passes on
 *   AMD with InvariantTSC set; informational CHECK on AMD without it.
 *   MEDIUM — informational / documentation.
 *
 * Reference: AMD64 APM Vol. 2 §7.13 (RDTSC), AMD64 APM Vol. 3 §2.4
 *            (CPUID 0x80000007 EDX[8] InvariantTSC).
 *
 * SWLSVROS-6556
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>
#include <sys/time.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tsc_utils.h"

/* -------------------------------------------------------------------------
 * File-local helpers
 * ---------------------------------------------------------------------- */

static int
tsc_get_ncpus(void)
{
	long n;

	n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n < 1 ? 1 : (int)n);
}

static int
tsc_pin_to_cpu(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask) == 0 ? 0 : -1);
}

/*
 * Thread argument for the simultaneous two-CPU TSC reader.
 * ready is written by the main thread; the child spins on it before
 * issuing tsc_read().  An atomic release/acquire pair ensures both
 * threads see the store before either fires.
 */
struct tsc_reader_arg {
	int		 cpu;
	uint64_t	 tsc;		/* filled by thread */
	volatile bool	*ready;		/* main sets true to release both */
	int		 pin_error;	/* errno if pin_to_cpu failed */
};

static void *
tsc_reader_thread(void *arg)
{
	struct tsc_reader_arg *a = arg;

	if (tsc_pin_to_cpu(a->cpu) != 0) {
		a->pin_error = errno;
		return (NULL);
	}
	/* Spin-wait for release.  pause avoids memory-order stalls. */
	while (!__atomic_load_n(a->ready, __ATOMIC_ACQUIRE))
		__asm__ volatile("pause" ::: "memory");

	a->tsc = tsc_read();
	return (NULL);
}

/*
 * One-shot thread: pin to cpu, read TSC, return in *out.
 * Used by the 1 000-iteration drift bound case.
 */
struct tsc_oneshot_arg {
	int	 cpu;
	uint64_t tsc;
	int	 error;
};

static void *
tsc_oneshot_thread(void *arg)
{
	struct tsc_oneshot_arg *a = arg;

	if (tsc_pin_to_cpu(a->cpu) != 0) {
		a->error = errno;
		return (NULL);
	}
	a->tsc = tsc_read();
	return (NULL);
}

/* Compute |a - b| for unsigned values without signed overflow. */
static uint64_t
tsc_abs_delta(uint64_t a, uint64_t b)
{
	return (a >= b ? a - b : b - a);
}

/* -------------------------------------------------------------------------
 * Common skip gate used by multi-CPU and InvariantTSC cases.
 * ---------------------------------------------------------------------- */
static void
tsc_skip_unless_cpuctl(void)
{
	int fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		atf_tc_skip("/dev/cpuctl0 not accessible: %s", strerror(errno));
	close(fd);
}

/* =========================================================================
 * TC-TSC-DRF-01  tsc_drift_same_cpu_zero
 * ====================================================================== */
ATF_TC(tsc_drift_same_cpu_zero);
ATF_TC_HEAD(tsc_drift_same_cpu_zero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DRF-01] Two consecutive tsc_read() calls on the same "
	    "pinned CPU (CPU 0).  Asserts no backwards jump and that the "
	    "inter-read delta is < 10 000 cycles (LFENCE overhead bound).");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_drift_same_cpu_zero, tc)
{
	uint64_t t0, t1, delta;
	int i, failures = 0;

	tsc_skip_unless_cpuctl();

	if (!tsc_feature_present())
		atf_tc_skip("TSC not present (CPUID.1.EDX[4] not set)");

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Failed to pin to CPU 0: %s", strerror(errno));

	for (i = 0; i < 1000; i++) {
		t0 = tsc_read();
		t1 = tsc_read();

		if (t1 < t0) {
			fprintf(stderr, "iter %d: TSC backwards "
			    "t0=%ju t1=%ju\n", i,
			    (uintmax_t)t0, (uintmax_t)t1);
			failures++;
		}
	}

	/* Use last pair for the delta check. */
	delta = t1 - t0;

	printf("Same-CPU consecutive TSC: last delta = %ju cycles\n",
	    (uintmax_t)delta);
	printf("Backwards jumps: %d / 1000\n", failures);

	ATF_REQUIRE_MSG(failures == 0,
	    "TSC moved backwards %d time(s) on a pinned CPU", failures);

	ATF_CHECK_MSG(delta < 10000,
	    "Same-CPU consecutive TSC delta %ju cycles exceeds 10 000 — "
	    "LFENCE overhead unexpectedly large", (uintmax_t)delta);
}

/* =========================================================================
 * TC-TSC-DRF-02  tsc_drift_two_cpu_monotonic
 * ====================================================================== */
ATF_TC(tsc_drift_two_cpu_monotonic);
ATF_TC_HEAD(tsc_drift_two_cpu_monotonic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DRF-02] Fire two threads simultaneously on CPU 0 and "
	    "CPU(ncpus-1); compare their TSC reads.  Asserts |delta| < "
	    "1 ms worth of TSC cycles.  On AMD EPYC bare metal the actual "
	    "delta is typically < 200 cycles.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "15");
}

ATF_TC_BODY(tsc_drift_two_cpu_monotonic, tc)
{
	struct tsc_reader_arg a0, a1;
	pthread_t t0, t1;
	volatile bool ready = false;
	uint64_t freq, threshold;
	int ncpus;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set — cross-CPU sync not "
		    "guaranteed");

	ncpus = tsc_get_ncpus();
	if (ncpus < 2)
		atf_tc_skip("Requires >= 2 online CPUs (found %d)", ncpus);

	freq = tsc_compute_frequency();
	if (freq == 0)
		freq = 3000000000ULL;		/* safe fallback: 3 GHz */

	/* 1 ms worth of cycles — very generous; actual is < 1 µs. */
	threshold = freq / 1000;

	/* Prepare args; both share the same ready flag. */
	memset(&a0, 0, sizeof(a0));
	memset(&a1, 0, sizeof(a1));
	a0.cpu   = 0;
	a1.cpu   = ncpus - 1;
	a0.ready = &ready;
	a1.ready = &ready;

	ATF_REQUIRE_MSG(pthread_create(&t0, NULL, tsc_reader_thread, &a0) == 0,
	    "pthread_create CPU 0 thread: %s", strerror(errno));
	ATF_REQUIRE_MSG(pthread_create(&t1, NULL, tsc_reader_thread, &a1) == 0,
	    "pthread_create CPU %d thread: %s", ncpus - 1, strerror(errno));

	/* Brief yield so both threads reach their spin-wait before release. */
	usleep(2000);

	/* Atomic release — both threads see the store before firing. */
	__atomic_store_n(&ready, true, __ATOMIC_RELEASE);

	pthread_join(t0, NULL);
	pthread_join(t1, NULL);

	ATF_REQUIRE_MSG(a0.pin_error == 0,
	    "CPU 0 thread pin failed: %s", strerror(a0.pin_error));
	ATF_REQUIRE_MSG(a1.pin_error == 0,
	    "CPU %d thread pin failed: %s", ncpus - 1, strerror(a1.pin_error));

	printf("CPU 0 TSC:   %ju\n", (uintmax_t)a0.tsc);
	printf("CPU %d TSC:  %ju\n", ncpus - 1, (uintmax_t)a1.tsc);
	printf("Signed delta (cpu%d - cpu0): %jd cycles\n",
	    ncpus - 1, (intmax_t)a1.tsc - (intmax_t)a0.tsc);
	printf("Threshold: %ju cycles (1 ms at %.2f GHz)\n",
	    (uintmax_t)threshold, (double)freq / 1e9);

	ATF_CHECK_MSG(tsc_abs_delta(a0.tsc, a1.tsc) < threshold,
	    "Cross-CPU TSC delta %ju cycles exceeds 1 ms threshold %ju "
	    "(cpu0=%ju cpu%d=%ju)",
	    (uintmax_t)tsc_abs_delta(a0.tsc, a1.tsc), (uintmax_t)threshold,
	    (uintmax_t)a0.tsc, ncpus - 1, (uintmax_t)a1.tsc);
}

/* =========================================================================
 * TC-TSC-DRF-03  tsc_drift_bound_1000_iterations
 * ====================================================================== */
ATF_TC(tsc_drift_bound_1000_iterations);
ATF_TC_HEAD(tsc_drift_bound_1000_iterations, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DRF-03] 1 000 paired TSC reads across CPU 0 and "
	    "CPU(ncpus-1) after sched_yield().  Tracks min/max/avg delta.  "
	    "Asserts max delta < 10 ms worth of cycles.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "60");
}

ATF_TC_BODY(tsc_drift_bound_1000_iterations, tc)
{
	struct tsc_oneshot_arg oa;
	pthread_t thr;
	uint64_t tsc_a, delta;
	uint64_t min_delta = UINT64_MAX, max_delta = 0, sum_delta = 0;
	uint64_t freq, threshold;
	int ncpus, i, errors = 0;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set");

	ncpus = tsc_get_ncpus();
	if (ncpus < 2)
		atf_tc_skip("Requires >= 2 online CPUs (found %d)", ncpus);

	freq = tsc_compute_frequency();
	if (freq == 0)
		freq = 3000000000ULL;

	/* 10 ms threshold — very conservative. */
	threshold = freq / 100;

	/* Pin main to CPU 0. */
	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	for (i = 0; i < 1000; i++) {
		tsc_a = tsc_read();

		/* Spawn one-shot reader on the far CPU. */
		memset(&oa, 0, sizeof(oa));
		oa.cpu = ncpus - 1;
		if (pthread_create(&thr, NULL, tsc_oneshot_thread, &oa) != 0) {
			errors++;
			continue;
		}
		pthread_join(thr, NULL);

		if (oa.error != 0) {
			errors++;
			continue;
		}

		delta = tsc_abs_delta(tsc_a, oa.tsc);
		if (delta < min_delta)
			min_delta = delta;
		if (delta > max_delta)
			max_delta = delta;
		sum_delta += delta;

		sched_yield();
	}

	printf("TSC cross-CPU drift over 1000 iterations:\n");
	printf("  min_delta  = %ju cycles\n", (uintmax_t)min_delta);
	printf("  max_delta  = %ju cycles\n", (uintmax_t)max_delta);
	if (1000 - errors > 0)
		printf("  avg_delta  = %ju cycles\n",
		    (uintmax_t)(sum_delta / (1000 - errors)));
	printf("  threshold  = %ju cycles (10 ms at %.2f GHz)\n",
	    (uintmax_t)threshold, (double)freq / 1e9);
	printf("  errors     = %d\n", errors);

	ATF_REQUIRE_MSG(errors < 10,
	    "Too many thread spawn errors (%d / 1000)", errors);

	ATF_CHECK_MSG(max_delta < threshold,
	    "Max cross-CPU TSC delta %ju cycles exceeds 10 ms threshold %ju",
	    (uintmax_t)max_delta, (uintmax_t)threshold);
}

/* =========================================================================
 * TC-TSC-DRF-04  tsc_drift_across_sleep
 * ====================================================================== */
ATF_TC(tsc_drift_across_sleep);
ATF_TC_HEAD(tsc_drift_across_sleep, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DRF-04] Read TSC around a 50 ms usleep().  Compute "
	    "implied TSC frequency from the elapsed wall clock and assert "
	    "it agrees with CPUID within 5%.  Validates TSC does not pause "
	    "in C-states during sleep.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "15");
}

ATF_TC_BODY(tsc_drift_across_sleep, tc)
{
	struct timespec w0, w1;
	uint64_t tsc0, tsc1, freq_cpuid, freq_measured;
	double elapsed_s, error_pct;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set — TSC may pause in C-states");

	freq_cpuid = tsc_compute_frequency();
	if (freq_cpuid == 0)
		atf_tc_skip("Cannot determine TSC frequency from CPUID");

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w0) == 0,
	    "clock_gettime: %s", strerror(errno));
	tsc0 = tsc_read();

	usleep(50000);		/* 50 ms */

	tsc1 = tsc_read();
	ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w1) == 0,
	    "clock_gettime: %s", strerror(errno));

	elapsed_s = (double)(w1.tv_sec - w0.tv_sec) +
	    (double)(w1.tv_nsec - w0.tv_nsec) / 1e9;

	if (elapsed_s <= 0.0)
		atf_tc_skip("Wall-clock elapsed <= 0 — clock resolution issue");

	freq_measured = (uint64_t)((double)(tsc1 - tsc0) / elapsed_s);
	error_pct = (freq_measured > freq_cpuid
	    ? (double)(freq_measured - freq_cpuid)
	    : (double)(freq_cpuid - freq_measured))
	    / (double)freq_cpuid * 100.0;

	printf("CPUID TSC frequency:    %.4f GHz\n",
	    (double)freq_cpuid / 1e9);
	printf("Measured TSC frequency: %.4f GHz "
	    "(tsc_delta=%ju  elapsed=%.4f s)\n",
	    (double)freq_measured / 1e9,
	    (uintmax_t)(tsc1 - tsc0), elapsed_s);
	printf("Agreement: %.4f%%  (threshold 5.00%%)\n", error_pct);

	ATF_CHECK_MSG(error_pct < 5.0,
	    "TSC frequency disagreement %.4f%% > 5%% across 50 ms sleep "
	    "(cpuid=%ju Hz  measured=%ju Hz)",
	    error_pct, (uintmax_t)freq_cpuid, (uintmax_t)freq_measured);
}

/* =========================================================================
 * TC-TSC-DRF-05  tsc_drift_amd_invariant_guarantee
 * ====================================================================== */
ATF_TC(tsc_drift_amd_invariant_guarantee);
ATF_TC_HEAD(tsc_drift_amd_invariant_guarantee, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DRF-05] Documents the AMD InvariantTSC CPUID guarantee "
	    "(CPUID.80000007.EDX[8]).  Skips on non-AMD.  CHECK (not REQUIRE) "
	    "on AMD — a soft assertion that warns on pre-Zen hardware without "
	    "the bit set.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_drift_amd_invariant_guarantee, tc)
{
	uint32_t regs[4];
	int error;

	tsc_skip_unless_cpuctl();

	if (!tsc_cpu_is_amd())
		atf_tc_skip("Not an AuthenticAMD CPU");

	if (tsc_max_ext_leaf() < TSC_CPUID_APM)
		atf_tc_skip("CPUID max extended leaf < 0x80000007");

	error = tsc_cpuid(TSC_CPUID_APM, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID 0x80000007 failed: %s", strerror(error));

	printf("AMD CPU family=0x%02x model=0x%02x stepping=0x%x\n",
	    tsc_cpu_family(), tsc_cpu_model(), tsc_cpu_stepping());
	printf("CPUID.80000007 EDX = 0x%08x\n", regs[3]);
	printf("InvariantTSC (EDX[8]): %s\n",
	    (regs[3] & TSC_INVARIANT_EDX_BIT) ? "SET" : "NOT SET");

	if ((regs[3] & TSC_INVARIANT_EDX_BIT) != 0) {
		printf("Cross-CPU TSC synchronisation guaranteed by hardware.\n");
	} else {
		printf("WARNING: AMD CPU without InvariantTSC — "
		    "pre-Zen hardware or restricted hypervisor.\n");
	}

	ATF_CHECK_MSG((regs[3] & TSC_INVARIANT_EDX_BIT) != 0,
	    "AMD CPU (family=0x%02x model=0x%02x) does not set "
	    "InvariantTSC — expected on Zen 1+",
	    tsc_cpu_family(), tsc_cpu_model());
}

/* =========================================================================
 * Test program registration
 * ====================================================================== */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tsc_drift_same_cpu_zero);
	ATF_TP_ADD_TC(tp, tsc_drift_two_cpu_monotonic);
	ATF_TP_ADD_TC(tp, tsc_drift_bound_1000_iterations);
	ATF_TP_ADD_TC(tp, tsc_drift_across_sleep);
	ATF_TP_ADD_TC(tp, tsc_drift_amd_invariant_guarantee);
	return (atf_no_error());
}

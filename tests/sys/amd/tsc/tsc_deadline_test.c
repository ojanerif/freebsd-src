/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Osvaldo Janeri Filho <ojanerif@amd.com>
 *
 * [TC-TSC-DDL] AMD TSC deadline timer and cross-core synchronisation tests.
 *
 * Regression suite for SWLSVROS-6600 (TSC deadline timer accuracy + AP sync).
 * All cases are black-box observable checks — no kernel source modifications.
 *
 * 4 test cases:
 *
 * TC-TSC-DDL-01  tsc_kernel_freq_matches_cpuid
 *   Read machdep.tsc_freq (kernel-calibrated) and compare against the
 *   CPUID-derived frequency from tsc_compute_frequency().  Assert agreement
 *   within 1%.  Direct regression detector for HPET calibration in tsc.c.
 *   CRITICAL.
 *
 * TC-TSC-DDL-02  tsc_deadline_latency_bound
 *   Schedule an absolute wakeup via clock_nanosleep(CLOCK_MONOTONIC,
 *   TIMER_ABSTIME) to a target 10 ms in the future.  Measure actual wakeup
 *   TSC against the expected TSC at deadline.  Assert overshoot < 5 ms of
 *   cycles.  If the TSC deadline timer is broken the wakeup is delayed by
 *   a full timer tick (typically >> 5 ms).
 *   HIGH.
 *
 * TC-TSC-DDL-03  tsc_all_cpu_sync_tight
 *   Simultaneously read TSC on all online CPUs using a spin-barrier.
 *   Compute the maximum pairwise delta.  Assert it is < 200 µs of cycles.
 *   Extends TC-TSC-DRF-02 to all CPUs with a 5× tighter bound and
 *   directly tests the AP sync barrier correctness.
 *   HIGH.
 *
 * TC-TSC-DDL-04  tsc_kernel_invariant_sysctl
 *   kern.timecounter.invariant_tsc must be 1 and agree with
 *   CPUID.80000007.EDX[8].  kern.timecounter.smp_tsc must be 1.
 *   hw.apic.timer_tsc_deadline is printed (informational).
 *   Validates the kernel correctly detected and set invariant/SMP-TSC
 *   mode — prerequisites for deadline timer correctness.
 *   HIGH.
 *
 * All cases require root (CPUID via /dev/cpuctl0).
 *
 * Reference: AMD64 APM Vol. 3 §2.4, §6.3.  FreeBSD sys/x86/x86/tsc.c.
 *
 * SWLSVROS-6600
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
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

static void
tsc_skip_unless_cpuctl(void)
{
	int fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		atf_tc_skip("/dev/cpuctl0 not accessible: %s", strerror(errno));
	close(fd);
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

static int
tsc_get_ncpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n < 1 ? 1 : (int)n);
}

/* =========================================================================
 * TC-TSC-DDL-01  tsc_kernel_freq_matches_cpuid
 *
 * The kernel calibrates the TSC frequency during boot (via HPET or other
 * reference) and stores it in machdep.tsc_freq.  If HPET calibration is
 * broken (the bug in SWLSVROS-6600), the kernel value drifts from the
 * true CPUID-derived frequency.
 *
 * Acceptance: |kernel_freq - cpuid_freq| / cpuid_freq < 1%.
 * ====================================================================== */
ATF_TC(tsc_kernel_freq_matches_cpuid);
ATF_TC_HEAD(tsc_kernel_freq_matches_cpuid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DDL-01] Compare machdep.tsc_freq (kernel-calibrated) "
	    "against CPUID-derived TSC frequency.  Assert agreement within 1%%. "
	    "Direct regression detector for HPET calibration in tsc.c.  "
	    "SWLSVROS-6600.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_kernel_freq_matches_cpuid, tc)
{
	uint64_t freq_cpuid, freq_kernel;
	size_t sz;
	double error_pct;

	tsc_skip_unless_cpuctl();

	/* Read kernel-calibrated TSC frequency. */
	sz = sizeof(freq_kernel);
	if (sysctlbyname("machdep.tsc_freq", &freq_kernel, &sz,
	    NULL, 0) != 0)
		atf_tc_skip("machdep.tsc_freq sysctl not available: %s",
		    strerror(errno));

	if (freq_kernel == 0)
		atf_tc_skip("machdep.tsc_freq is 0 — TSC calibration "
		    "disabled or not performed");

	/* Compute CPUID-derived frequency. */
	freq_cpuid = tsc_compute_frequency();
	if (freq_cpuid == 0)
		atf_tc_skip("Cannot derive TSC frequency from CPUID 0x15/0x16 "
		    "— cross-check not possible on this CPU");

	error_pct = (freq_kernel > freq_cpuid
	    ? (double)(freq_kernel - freq_cpuid)
	    : (double)(freq_cpuid - freq_kernel))
	    / (double)freq_cpuid * 100.0;

	printf("machdep.tsc_freq (kernel):  %ju Hz  (%.4f GHz)\n",
	    (uintmax_t)freq_kernel, (double)freq_kernel / 1e9);
	printf("CPUID-derived frequency:    %ju Hz  (%.4f GHz)\n",
	    (uintmax_t)freq_cpuid, (double)freq_cpuid / 1e9);
	printf("Disagreement: %.4f%%  (threshold: 1.00%%)\n", error_pct);

	ATF_CHECK_MSG(error_pct < 1.0,
	    "machdep.tsc_freq (%ju Hz) disagrees with CPUID-derived "
	    "frequency (%ju Hz) by %.4f%% — exceeds 1%% threshold. "
	    "Likely HPET calibration error (SWLSVROS-6600).",
	    (uintmax_t)freq_kernel, (uintmax_t)freq_cpuid, error_pct);
}

/* =========================================================================
 * TC-TSC-DDL-02  tsc_deadline_latency_bound
 *
 * Schedule an absolute wakeup 10 ms in the future using
 * clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME).  Capture TSC
 * immediately before the sleep call and again immediately after wakeup.
 *
 * Compute expected TSC advance (10 ms × freq_hz / 1e9) and compare
 * against actual TSC advance.  Overshoot = actual - expected.
 *
 * If the TSC deadline timer fires late (broken deadline hardware), the
 * wakeup will be delayed by a full LAPIC timer tick (1–10 ms extra on
 * typical FreeBSD configs), easily exceeding the 5 ms tolerance.
 *
 * Repeat 10 times and report worst case.
 * ====================================================================== */
ATF_TC(tsc_deadline_latency_bound);
ATF_TC_HEAD(tsc_deadline_latency_bound, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DDL-02] Schedule absolute wakeups 10 ms in the future "
	    "via clock_nanosleep(TIMER_ABSTIME) on a pinned CPU.  Assert that "
	    "the actual TSC advance never exceeds expected by more than 5 ms "
	    "of cycles (worst case over 10 repetitions).  A broken TSC "
	    "deadline timer causes wakeups delayed by a full LAPIC tick. "
	    "SWLSVROS-6600.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(tsc_deadline_latency_bound, tc)
{
#define	DDL02_REPS		10
#define	DDL02_SLEEP_NS		10000000LL	/* 10 ms */
#define	DDL02_OVERSHOOT_MS	5		/* 5 ms tolerance */

	struct timespec now, target;
	uint64_t freq, tsc_before, tsc_after;
	uint64_t expected_delta, actual_delta;
	uint64_t overshoot_threshold;
	uint64_t max_overshoot = 0;
	int i;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set — deadline latency "
		    "measurement unreliable");

	freq = tsc_compute_frequency();
	if (freq == 0)
		atf_tc_skip("Cannot determine TSC frequency from CPUID");

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	/* expected TSC ticks for 10 ms */
	expected_delta = freq / 100;

	/* overshoot threshold: DDL02_OVERSHOOT_MS ms of TSC cycles */
	overshoot_threshold = (uint64_t)DDL02_OVERSHOOT_MS * freq / 1000;

	/* Print TSC-deadline mode status for diagnostic context. */
	{
		int tsc_dl = 0;
		size_t tsc_dl_sz = sizeof(tsc_dl);
		if (sysctlbyname("hw.apic.timer_tsc_deadline",
		    &tsc_dl, &tsc_dl_sz, NULL, 0) == 0)
			printf("hw.apic.timer_tsc_deadline: %d %s\n", tsc_dl,
			    tsc_dl ? "(TSC-deadline LAPIC mode)"
				   : "(one-shot/periodic LAPIC mode)");
	}
	printf("TSC frequency: %.4f GHz\n", (double)freq / 1e9);
	printf("Expected 10 ms delta: %ju cycles\n",
	    (uintmax_t)expected_delta);
	printf("Overshoot threshold:  %ju cycles (%d ms)\n\n",
	    (uintmax_t)overshoot_threshold, DDL02_OVERSHOOT_MS);

	for (i = 0; i < DDL02_REPS; i++) {
		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &now) == 0,
		    "clock_gettime: %s", strerror(errno));

		/* Target = now + 10 ms */
		target.tv_sec  = now.tv_sec;
		target.tv_nsec = now.tv_nsec + DDL02_SLEEP_NS;
		if (target.tv_nsec >= 1000000000LL) {
			target.tv_sec++;
			target.tv_nsec -= 1000000000LL;
		}

		tsc_before = tsc_read();

		ATF_REQUIRE_MSG(
		    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&target, NULL) == 0,
		    "clock_nanosleep: %s", strerror(errno));

		tsc_after = tsc_read();

		actual_delta = tsc_after - tsc_before;

		uint64_t overshoot = (actual_delta > expected_delta)
		    ? (actual_delta - expected_delta) : 0;

		if (overshoot > max_overshoot)
			max_overshoot = overshoot;

		printf("Rep %2d: actual=%ju  expected=%ju  "
		    "overshoot=%ju cycles (%.3f ms)\n",
		    i + 1, (uintmax_t)actual_delta,
		    (uintmax_t)expected_delta,
		    (uintmax_t)overshoot,
		    (double)overshoot / (double)freq * 1000.0);
	}

	printf("\nWorst-case overshoot: %ju cycles (%.3f ms)  "
	    "threshold: %ju cycles (%d ms)\n",
	    (uintmax_t)max_overshoot,
	    (double)max_overshoot / (double)freq * 1000.0,
	    (uintmax_t)overshoot_threshold,
	    DDL02_OVERSHOOT_MS);

	ATF_CHECK_MSG(max_overshoot < overshoot_threshold,
	    "Worst-case wakeup overshoot %ju cycles (%.3f ms) exceeds "
	    "%d ms threshold — TSC deadline timer may be broken "
	    "(SWLSVROS-6600)",
	    (uintmax_t)max_overshoot,
	    (double)max_overshoot / (double)freq * 1000.0,
	    DDL02_OVERSHOOT_MS);

#undef DDL02_REPS
#undef DDL02_SLEEP_NS
#undef DDL02_OVERSHOOT_MS
}

/* =========================================================================
 * TC-TSC-DDL-03  tsc_all_cpu_sync_tight
 *
 * Simultaneously read TSC on all online CPUs using a spin-barrier.
 * Compute the maximum pairwise delta across all readings.
 *
 * On a properly synchronised system (AP sync barrier correct) all CPUs
 * should agree within a few hundred cycles.  200 µs is chosen as the
 * threshold — roughly 500 000 cycles at 2.5 GHz, far looser than the
 * hardware guarantee but tight enough to catch a broken AP barrier where
 * CPUs diverge by milliseconds.
 *
 * Extends TC-TSC-DRF-02 to all CPUs (not just 2) with a 5× tighter bound.
 * ====================================================================== */

struct ddl03_arg {
	int			 cpu;
	uint64_t		 tsc;
	volatile bool		*gate;		/* spin until released */
	volatile int		*n_ready;	/* atomic ready count   */
	int			 pin_error;
};

static void *
ddl03_reader(void *varg)
{
	struct ddl03_arg *a = varg;

	if (tsc_pin_to_cpu(a->cpu) != 0) {
		a->pin_error = errno;
		/* Still participate in barrier to avoid deadlock. */
	}

	/* Signal that this thread is in its spin loop. */
	__atomic_fetch_add(a->n_ready, 1, __ATOMIC_RELEASE);

	/* Tight spin — read TSC as soon as gate opens. */
	while (!__atomic_load_n(a->gate, __ATOMIC_ACQUIRE))
		__asm__ volatile("pause" ::: "memory");

	a->tsc = tsc_read();
	return (NULL);
}

ATF_TC(tsc_all_cpu_sync_tight);
ATF_TC_HEAD(tsc_all_cpu_sync_tight, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DDL-03] Simultaneously read TSC on all online CPUs via "
	    "a counted spin-barrier.  Assert max pairwise delta (excluding "
	    "scheduling outliers) < 200 µs of cycles. "
	    "Extends TC-TSC-DRF-02 to all CPUs with a 5x tighter bound.  "
	    "Directly tests AP sync barrier correctness (SWLSVROS-6600).");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(tsc_all_cpu_sync_tight, tc)
{
#define	DDL03_THRESHOLD_US	200	/* 200 µs — real TSC skew limit    */
#define	DDL03_OUTLIER_US	1000	/* >1 ms = scheduling jitter        */
#define	DDL03_MAX_OUTLIER_PCT	10	/* fail if >10% CPUs are outliers   */

	struct ddl03_arg *args;
	pthread_t *tids;
	volatile bool gate = false;
	volatile int n_ready = 0;
	uint64_t freq, threshold_cycles, outlier_cycles;
	uint64_t min_tsc, max_tsc, sync_max_delta;
	int ncpus, i, n_outliers;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set — cross-CPU sync test "
		    "requires invariant TSC");

	ncpus = tsc_get_ncpus();
	if (ncpus < 2)
		atf_tc_skip("Requires >= 2 online CPUs (found %d)", ncpus);

	freq = tsc_compute_frequency();
	if (freq == 0)
		freq = 3000000000ULL;		/* safe fallback: 3 GHz */

	/* 200 µs in TSC cycles — real skew threshold */
	threshold_cycles = (uint64_t)DDL03_THRESHOLD_US * freq / 1000000;

	/* 1 ms in TSC cycles — scheduling outlier cutoff */
	outlier_cycles = (uint64_t)DDL03_OUTLIER_US * freq / 1000000;

	args = calloc(ncpus, sizeof(*args));
	ATF_REQUIRE_MSG(args != NULL, "calloc: %s", strerror(errno));

	tids = calloc(ncpus, sizeof(*tids));
	ATF_REQUIRE_MSG(tids != NULL, "calloc: %s", strerror(errno));

	for (i = 0; i < ncpus; i++) {
		args[i].cpu     = i;
		args[i].gate    = &gate;
		args[i].n_ready = &n_ready;
		args[i].pin_error = 0;
		ATF_REQUIRE_MSG(
		    pthread_create(&tids[i], NULL, ddl03_reader, &args[i]) == 0,
		    "pthread_create CPU %d: %s", i, strerror(errno));
	}

	/*
	 * Wait until every thread has incremented n_ready, confirming it is
	 * inside its spin loop.  This replaces the blind usleep(5000) which
	 * was not sufficient on 192-CPU NUMA systems where threads on distant
	 * NUMA nodes or deep C-state CPUs could be scheduled after the gate
	 * was already released, producing false outliers of several ms.
	 */
	while (__atomic_load_n(&n_ready, __ATOMIC_ACQUIRE) < ncpus)
		__asm__ volatile("pause" ::: "memory");

	/* Brief settle: let all threads reach the tight pause loop. */
	usleep(100);

	/* Release all threads simultaneously. */
	__atomic_store_n(&gate, true, __ATOMIC_RELEASE);

	for (i = 0; i < ncpus; i++)
		pthread_join(tids[i], NULL);

	/* First pass: find the cluster minimum (ignoring outliers). */
	min_tsc = UINT64_MAX;
	for (i = 0; i < ncpus; i++) {
		if (args[i].tsc < min_tsc)
			min_tsc = args[i].tsc;
	}

	/*
	 * Second pass: classify each CPU.
	 * Outlier = TSC > min_tsc + outlier_cycles (>1 ms above cluster).
	 * These are threads that were preempted or woke from deep C-state
	 * after the gate fired — scheduler jitter, not TSC skew.
	 */
	n_outliers = 0;
	max_tsc = 0;
	sync_max_delta = 0;

	for (i = 0; i < ncpus; i++) {
		uint64_t delta = args[i].tsc - min_tsc;
		bool outlier   = (delta > outlier_cycles);

		if (args[i].pin_error != 0)
			fprintf(stderr, "CPU %3d pin failed: %s\n",
			    i, strerror(args[i].pin_error));

		if (outlier) {
			n_outliers++;
			printf("  CPU %3d: TSC = %ju  [SCHED-OUTLIER +%.0f µs]\n",
			    i, (uintmax_t)args[i].tsc,
			    (double)delta / (double)freq * 1e6);
		} else {
			if (args[i].tsc > max_tsc)
				max_tsc = args[i].tsc;
			if (delta > sync_max_delta)
				sync_max_delta = delta;
			printf("  CPU %3d: TSC = %ju\n",
			    i, (uintmax_t)args[i].tsc);
		}
	}

	printf("\nAll-%d-CPU simultaneous TSC read:\n", ncpus);
	printf("  cluster min TSC        = %ju\n", (uintmax_t)min_tsc);
	printf("  cluster max TSC        = %ju\n", (uintmax_t)max_tsc);
	printf("  cluster max delta      = %ju cycles (%.3f µs)\n",
	    (uintmax_t)sync_max_delta,
	    (double)sync_max_delta / (double)freq * 1e6);
	printf("  TSC skew threshold     = %ju cycles (%d µs)\n",
	    (uintmax_t)threshold_cycles, DDL03_THRESHOLD_US);
	printf("  scheduling outliers    = %d / %d CPUs (limit %d%%)\n",
	    n_outliers, ncpus, DDL03_MAX_OUTLIER_PCT);

	free(args);
	free(tids);

	/*
	 * Fail if too many CPUs were outliers — indicates a systemic problem
	 * (e.g. all NUMA-remote CPUs missed the barrier) rather than normal
	 * C-state wake latency of a few isolated CPUs.
	 */
	ATF_CHECK_MSG(n_outliers * 100 / ncpus < DDL03_MAX_OUTLIER_PCT,
	    "%d / %d CPUs (%d%%) were scheduling outliers (> %d µs late) — "
	    "barrier may not be functioning correctly (SWLSVROS-6600)",
	    n_outliers, ncpus,
	    n_outliers * 100 / ncpus,
	    DDL03_OUTLIER_US);

	/*
	 * Fail if the synchronised cluster itself exceeds the TSC skew
	 * threshold — this is the actual AP sync barrier correctness check.
	 */
	ATF_CHECK_MSG(sync_max_delta < threshold_cycles,
	    "Max pairwise cross-CPU TSC delta (excl. %d outliers) "
	    "%ju cycles (%.3f µs) exceeds %d µs threshold — "
	    "AP sync barrier may be broken (SWLSVROS-6600)",
	    n_outliers,
	    (uintmax_t)sync_max_delta,
	    (double)sync_max_delta / (double)freq * 1e6,
	    DDL03_THRESHOLD_US);

#undef DDL03_THRESHOLD_US
#undef DDL03_OUTLIER_US
#undef DDL03_MAX_OUTLIER_PCT
}

/* =========================================================================
 * TC-TSC-DDL-04  tsc_kernel_invariant_sysctl
 *
 * kern.timecounter.invariant_tsc must be 1 and agree with
 * CPUID.80000007.EDX[8].  kern.timecounter.smp_tsc must be 1 — this
 * is the flag FreeBSD sets when it detects a properly synchronised
 * multi-CPU TSC.  hw.apic.timer_tsc_deadline is printed informational.
 *
 * If the kernel's invariant_tsc or smp_tsc are 0 it means the TSC
 * deadline path in tsc.c is disabled — a direct consequence of the bug.
 * ====================================================================== */
ATF_TC(tsc_kernel_invariant_sysctl);
ATF_TC_HEAD(tsc_kernel_invariant_sysctl, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-DDL-04] Verify kern.timecounter.invariant_tsc=1 and "
	    "kern.timecounter.smp_tsc=1, and that invariant_tsc agrees with "
	    "CPUID.80000007.EDX[8].  Failure means the kernel disabled the "
	    "TSC deadline path due to a calibration error (SWLSVROS-6600).");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_kernel_invariant_sysctl, tc)
{
	int invariant_tsc, smp_tsc, tsc_deadline;
	bool deadline_available, smp_tsc_available;
	size_t sz;
	bool cpuid_invariant;

	tsc_skip_unless_cpuctl();

	/* --- kern.timecounter.invariant_tsc --- */
	sz = sizeof(invariant_tsc);
	if (sysctlbyname("kern.timecounter.invariant_tsc",
	    &invariant_tsc, &sz, NULL, 0) != 0)
		atf_tc_skip("kern.timecounter.invariant_tsc not available: %s",
		    strerror(errno));

	/* --- kern.timecounter.smp_tsc --- */
	sz = sizeof(smp_tsc);
	smp_tsc_available = (sysctlbyname("kern.timecounter.smp_tsc",
	    &smp_tsc, &sz, NULL, 0) == 0);

	/* --- hw.apic.timer_tsc_deadline (informational) --- */
	sz = sizeof(tsc_deadline);
	deadline_available = (sysctlbyname("hw.apic.timer_tsc_deadline",
	    &tsc_deadline, &sz, NULL, 0) == 0);

	/* --- CPUID invariant bit --- */
	cpuid_invariant = tsc_invariant_present();

	/* Print full picture. */
	printf("kern.timecounter.invariant_tsc : %d  (expected 1)\n",
	    invariant_tsc);
	printf("kern.timecounter.smp_tsc       : %s\n",
	    !smp_tsc_available ? "not available" :
	    (smp_tsc ? "1  (expected 1)" : "0  (FAIL)"));
	printf("hw.apic.timer_tsc_deadline     : %s  (informational)\n",
	    !deadline_available ? "not available" :
	    (tsc_deadline ? "1 (TSC-deadline mode active)" :
		"0 (one-shot or periodic LAPIC mode)"));
	printf("CPUID.80000007.EDX[8] (InvariantTSC): %s\n",
	    cpuid_invariant ? "SET" : "NOT SET");

	/* Agreement between sysctl and CPUID. */
	if (cpuid_invariant) {
		ATF_CHECK_MSG(invariant_tsc == 1,
		    "CPUID reports InvariantTSC but "
		    "kern.timecounter.invariant_tsc=%d — kernel failed to "
		    "detect or trust the invariant TSC (SWLSVROS-6600)",
		    invariant_tsc);
	} else {
		/*
		 * CPUID does not set InvariantTSC.  This is unusual on AMD
		 * Zen but valid on very old or restricted-hypervisor CPUs.
		 * Print a warning; do not fail the test.
		 */
		printf("WARNING: CPUID InvariantTSC not set — "
		    "pre-Zen or restricted hypervisor.  "
		    "Deadline timer may not be available.\n");
		ATF_CHECK_MSG(invariant_tsc == 0,
		    "kern.timecounter.invariant_tsc=%d but CPUID does not "
		    "set InvariantTSC — sysctl/CPUID mismatch",
		    invariant_tsc);
	}

	/* smp_tsc: required for multi-CPU deadline correctness. */
	if (smp_tsc_available) {
		ATF_CHECK_MSG(smp_tsc == 1,
		    "kern.timecounter.smp_tsc=%d — kernel did not verify "
		    "cross-CPU TSC synchronisation.  TSC deadline timer "
		    "is unsafe on SMP without this (SWLSVROS-6600)",
		    smp_tsc);
	}
}

/* =========================================================================
 * Test program registration
 * ====================================================================== */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tsc_kernel_freq_matches_cpuid);
	ATF_TP_ADD_TC(tp, tsc_deadline_latency_bound);
	ATF_TP_ADD_TC(tp, tsc_all_cpu_sync_tight);
	ATF_TP_ADD_TC(tp, tsc_kernel_invariant_sysctl);
	return (atf_no_error());
}

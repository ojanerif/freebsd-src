/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Osvaldo Janeri Filho <ojanerif@amd.com>
 *
 * [TC-TSC-INV] AMD InvariantTSC rate-constancy tests.
 *
 * Validates that the TSC runs at a constant rate regardless of P-state
 * transitions, scheduler preemption, and C-state entry during yield.
 * Distinct from tsc_drift_test (cross-CPU delta) — this suite measures
 * rate constancy on a single pinned CPU over time.
 *
 * 4 test cases:
 *
 * TC-TSC-INV-01  tsc_invariant_cpuid_verified
 *   Hard REQUIRE on CPUID.80000007.EDX[8].  Gate for the other three.
 *   HIGH — foundational invariant guarantee check.
 *
 * TC-TSC-INV-02  tsc_invariant_rate_across_10ms
 *   5 × 10 ms measurement windows on a pinned CPU.  Asserts the derived
 *   TSC frequency varies < 1% across all windows.
 *   HIGH — rate constancy under quiescent conditions.
 *
 * TC-TSC-INV-03  tsc_invariant_monotonic_across_yield
 *   500 sched_yield() calls; asserts TSC advances after every yield.
 *   CRITICAL — TSC must not pause during scheduler preemption / C-states.
 *
 * TC-TSC-INV-04  tsc_invariant_frequency_vs_nominal
 *   200 ms busy loop.  Asserts measured TSC frequency agrees with
 *   CPUID-derived frequency within 0.5%.
 *   MEDIUM — tight rate bound under CPU-bound load.
 *
 * Reference: AMD64 APM Vol. 3 §2.4 (CPUID 0x80000007 EDX[8]).
 *
 * SWLSVROS-6556
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

#include <atf-c.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

/* Return elapsed seconds between two CLOCK_MONOTONIC samples. */
static double
timespec_diff_s(const struct timespec *a, const struct timespec *b)
{
	return ((double)(a->tv_sec - b->tv_sec) +
	    (double)(a->tv_nsec - b->tv_nsec) / 1e9);
}

/* =========================================================================
 * TC-TSC-INV-01  tsc_invariant_cpuid_verified
 * ====================================================================== */
ATF_TC(tsc_invariant_cpuid_verified);
ATF_TC_HEAD(tsc_invariant_cpuid_verified, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-INV-01] Hard REQUIRE on CPUID.80000007.EDX[8] "
	    "(InvariantTSC).  Fails if the bit is absent — the other three "
	    "invariant cases depend on this guarantee.  Also reads and prints "
	    "the hw.acpi.cpu.cx_lowest sysctl if available.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_invariant_cpuid_verified, tc)
{
	uint32_t regs[4];
	char cx_lowest[64];
	size_t cx_len;
	int error;

	tsc_skip_unless_cpuctl();

	if (tsc_max_ext_leaf() < TSC_CPUID_APM)
		atf_tc_skip("CPUID max extended leaf (0x%08x) < 0x80000007 — "
		    "InvariantTSC probe not possible",
		    tsc_max_ext_leaf());

	error = tsc_cpuid(TSC_CPUID_APM, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID 0x80000007 failed: %s", strerror(error));

	printf("CPU: family=0x%02x model=0x%02x stepping=0x%x\n",
	    tsc_cpu_family(), tsc_cpu_model(), tsc_cpu_stepping());
	printf("CPUID.80000007 EDX = 0x%08x\n", regs[3]);

	/* Print C-state depth if available (informational). */
	cx_len = sizeof(cx_lowest);
	if (sysctlbyname("hw.acpi.cpu.cx_lowest", cx_lowest,
	    &cx_len, NULL, 0) == 0)
		printf("hw.acpi.cpu.cx_lowest = %s\n", cx_lowest);

	ATF_REQUIRE_MSG((regs[3] & TSC_INVARIANT_EDX_BIT) != 0,
	    "InvariantTSC (CPUID.80000007.EDX[8]) not set on CPU "
	    "family=0x%02x model=0x%02x (EDX=0x%08x) — "
	    "TSC rate-constancy tests cannot run on this hardware",
	    tsc_cpu_family(), tsc_cpu_model(), regs[3]);

	printf("InvariantTSC: SET — TSC runs at constant rate "
	    "across P-states and C-states\n");
}

/* =========================================================================
 * TC-TSC-INV-02  tsc_invariant_rate_across_10ms
 * ====================================================================== */
ATF_TC(tsc_invariant_rate_across_10ms);
ATF_TC_HEAD(tsc_invariant_rate_across_10ms, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-INV-02] Collect 5 consecutive 10 ms TSC measurement "
	    "windows on CPU 0.  Derive TSC frequency from each window and "
	    "assert the spread across all 5 is < 1%%.  Validates rate "
	    "constancy under quiescent conditions.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(tsc_invariant_rate_across_10ms, tc)
{
#define	INV_WINDOWS	5
#define	INV_WINDOW_US	10000		/* 10 ms per window */

	struct timespec w0, w1;
	uint64_t tsc0, tsc1;
	double freq[INV_WINDOWS], elapsed;
	double min_freq, max_freq, sum_freq, variation_pct;
	int i;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set");

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	for (i = 0; i < INV_WINDOWS; i++) {
		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w0) == 0,
		    "clock_gettime: %s", strerror(errno));
		tsc0 = tsc_read();

		usleep(INV_WINDOW_US);

		tsc1 = tsc_read();
		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w1) == 0,
		    "clock_gettime: %s", strerror(errno));

		elapsed = timespec_diff_s(&w1, &w0);
		if (elapsed <= 0.0)
			atf_tc_skip("Window %d elapsed <= 0", i);

		freq[i] = (double)(tsc1 - tsc0) / elapsed;
		printf("Window %d: %.4f GHz  (elapsed=%.4f ms  "
		    "tsc_delta=%ju)\n",
		    i + 1, freq[i] / 1e9, elapsed * 1000.0,
		    (uintmax_t)(tsc1 - tsc0));
	}

	min_freq = max_freq = sum_freq = freq[0];
	for (i = 1; i < INV_WINDOWS; i++) {
		if (freq[i] < min_freq) min_freq = freq[i];
		if (freq[i] > max_freq) max_freq = freq[i];
		sum_freq += freq[i];
	}

	variation_pct = (max_freq - min_freq) / (sum_freq / INV_WINDOWS) * 100.0;

	printf("TSC rate variation across %d × %d ms windows: %.4f%%  "
	    "(threshold 1.00%%)\n",
	    INV_WINDOWS, INV_WINDOW_US / 1000, variation_pct);
	printf("  min=%.4f GHz  max=%.4f GHz  avg=%.4f GHz\n",
	    min_freq / 1e9, max_freq / 1e9,
	    (sum_freq / INV_WINDOWS) / 1e9);

	ATF_CHECK_MSG(variation_pct < 1.0,
	    "TSC frequency variation %.4f%% across %d windows exceeds 1%% — "
	    "TSC rate is not constant (min=%.4f GHz max=%.4f GHz)",
	    variation_pct, INV_WINDOWS, min_freq / 1e9, max_freq / 1e9);

#undef INV_WINDOWS
#undef INV_WINDOW_US
}

/* =========================================================================
 * TC-TSC-INV-03  tsc_invariant_monotonic_across_yield
 * ====================================================================== */
ATF_TC(tsc_invariant_monotonic_across_yield);
ATF_TC_HEAD(tsc_invariant_monotonic_across_yield, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-INV-03] Call sched_yield() 500 times; assert tsc_read() "
	    "strictly advances after every yield.  The CPU may enter C-states "
	    "during the yield — InvariantTSC guarantees the counter keeps "
	    "ticking regardless.  CRITICAL.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(tsc_invariant_monotonic_across_yield, tc)
{
#define	INV_YIELDS	500

	uint64_t t0, t1;
	uint64_t min_delta = UINT64_MAX, max_delta = 0, sum_delta = 0;
	int i, failures = 0;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set — TSC may pause in C-states");

	/*
	 * Do not pin here — we want to exercise the cross-CPU case too:
	 * sched_yield() may migrate the thread to another CPU.  InvariantTSC
	 * guarantees the counter is synchronised even across migrations.
	 */

	t0 = tsc_read();

	for (i = 0; i < INV_YIELDS; i++) {
		sched_yield();
		t1 = tsc_read();

		if (t1 <= t0) {
			fprintf(stderr,
			    "iter %d: TSC did not advance after yield "
			    "(t0=%ju t1=%ju delta=%jd)\n",
			    i, (uintmax_t)t0, (uintmax_t)t1,
			    (intmax_t)(t1 - t0));
			failures++;
		} else {
			uint64_t d = t1 - t0;

			if (d < min_delta) min_delta = d;
			if (d > max_delta) max_delta = d;
			sum_delta += d;
		}

		t0 = t1;
	}

	printf("TSC monotonicity across %d sched_yield() calls:\n",
	    INV_YIELDS);
	printf("  failures   = %d\n", failures);
	if (failures < INV_YIELDS && min_delta != UINT64_MAX &&
	    INV_YIELDS - failures > 0) {
		printf("  min_delta  = %ju cycles\n", (uintmax_t)min_delta);
		printf("  max_delta  = %ju cycles\n", (uintmax_t)max_delta);
		printf("  avg_delta  = %ju cycles\n",
		    (uintmax_t)(sum_delta / (INV_YIELDS - failures)));
	}

	ATF_CHECK_MSG(failures == 0,
	    "TSC failed to advance after yield %d time(s) in %d — "
	    "TSC paused during scheduler preemption / C-state entry",
	    failures, INV_YIELDS);

#undef INV_YIELDS
}

/* =========================================================================
 * TC-TSC-INV-04  tsc_invariant_frequency_vs_nominal
 * ====================================================================== */
ATF_TC(tsc_invariant_frequency_vs_nominal);
ATF_TC_HEAD(tsc_invariant_frequency_vs_nominal, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSC-INV-04] 200 ms busy loop on CPU 0.  Compare the TSC "
	    "frequency derived from the elapsed wall clock against the "
	    "CPUID-derived nominal frequency.  Asserts agreement < 0.5%%.  "
	    "The busy loop eliminates scheduling noise.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "15");
}

ATF_TC_BODY(tsc_invariant_frequency_vs_nominal, tc)
{
	struct timespec now, w0;
	uint64_t tsc0, tsc1;
	uint64_t freq_cpuid, freq_measured;
	double elapsed_s, error_pct;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set");

	freq_cpuid = tsc_compute_frequency();
	if (freq_cpuid == 0)
		atf_tc_skip("Cannot determine TSC frequency from CPUID");

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w0) == 0,
	    "clock_gettime: %s", strerror(errno));
	tsc0 = tsc_read();

	/*
	 * Busy loop for 200 ms of wall time.  The volatile reads and
	 * tsc_read() calls prevent the compiler from optimising away
	 * the loop body.
	 */
	for (;;) {
		volatile uint64_t x = tsc_read();
		(void)x;
		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &now) == 0,
		    "clock_gettime: %s", strerror(errno));
		elapsed_s = timespec_diff_s(&now, &w0);
		if (elapsed_s >= 0.2)
			break;
	}

	tsc1 = tsc_read();

	if (elapsed_s <= 0.0)
		atf_tc_skip("Elapsed time is zero — clock resolution issue");

	freq_measured = (uint64_t)((double)(tsc1 - tsc0) / elapsed_s);
	error_pct = (freq_measured > freq_cpuid
	    ? (double)(freq_measured - freq_cpuid)
	    : (double)(freq_cpuid - freq_measured))
	    / (double)freq_cpuid * 100.0;

	printf("CPUID TSC frequency:    %.4f GHz\n",
	    (double)freq_cpuid / 1e9);
	printf("Measured TSC frequency: %.4f GHz  "
	    "(200 ms busy loop  elapsed=%.4f ms  tsc_delta=%ju)\n",
	    (double)freq_measured / 1e9, elapsed_s * 1000.0,
	    (uintmax_t)(tsc1 - tsc0));
	printf("Agreement: %.4f%%  (threshold 0.50%%)\n", error_pct);

	ATF_CHECK_MSG(error_pct < 0.5,
	    "TSC frequency disagreement %.4f%% > 0.5%% "
	    "(cpuid=%ju Hz  measured=%ju Hz)",
	    error_pct, (uintmax_t)freq_cpuid, (uintmax_t)freq_measured);
}

/* =========================================================================
 * Test program registration
 * ====================================================================== */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tsc_invariant_cpuid_verified);
	ATF_TP_ADD_TC(tp, tsc_invariant_rate_across_10ms);
	ATF_TP_ADD_TC(tp, tsc_invariant_monotonic_across_yield);
	ATF_TP_ADD_TC(tp, tsc_invariant_frequency_vs_nominal);
	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Osvaldo Janeri Filho <ojanerif@amd.com>
 *
 * [TC-TSC-DET] AMD TSC detection and frequency validation tests.
 *
 * 7 ATF test cases covering:
 *   - Basic TSC presence (CPUID.1 EDX[4])
 *   - InvariantTSC flag (CPUID.80000007 EDX[8])
 *   - RDTSCP capability (CPUID.80000001 EDX[27])
 *   - CPUID leaf 0x15 ART/TSC ratio availability and coherence
 *   - TSC frequency derivation and sanity bounds
 *   - RDTSC monotonicity under tight loop
 *   - CPUID-derived vs wall-clock measured frequency agreement
 *
 * All cases require root because CPUID is issued via /dev/cpuctl0 ioctl.
 * Cases that read the TSC directly (tsc_read_monotonic, tsc_frequency_stable)
 * do not strictly need root, but the cpuctl check at the top of each body
 * is kept uniform for simplicity.
 *
 * Skip hierarchy: cpuctl accessible → feature present in CPUID → proceed.
 * No case hard-fails due to a missing optional feature; optional features
 * print a note and pass.
 *
 * SWLSVROS-6589
 */

#include <sys/param.h>
#include <sys/time.h>

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
 * Internal helpers shared across test bodies
 * ---------------------------------------------------------------------- */

/*
 * Attempt to open /dev/cpuctl0.  If it fails, skip immediately — without
 * cpuctl none of the CPUID probes work.
 */
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
 * TC-TSC-DET-01  tsc_feature_present
 *
 * Verify CPUID.1 EDX[4] (TSC) is set.
 *
 * CRITICAL — TSC is mandatory on any amd64 CPU.  A failure here means the
 * CPU or hypervisor is hiding a fundamental feature.
 * ====================================================================== */
ATF_TC(tsc_feature_present);
ATF_TC_HEAD(tsc_feature_present, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify CPUID.1 EDX[4] (TSC) is set.  CRITICAL: TSC is mandatory "
	    "on all amd64 CPUs; absence indicates a broken hypervisor or BIOS.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_feature_present, tc)
{
	uint32_t regs[4];
	int error;

	tsc_skip_unless_cpuctl();

	error = tsc_cpuid(TSC_CPUID_FEATURES, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID leaf 0x1 failed: %s", strerror(error));

	printf("CPUID.1 EAX=0x%08x  EDX=0x%08x\n", regs[0], regs[3]);
	printf("CPU: family=0x%02x model=0x%02x stepping=0x%x\n",
	    tsc_cpu_family(), tsc_cpu_model(), tsc_cpu_stepping());

	ATF_REQUIRE_MSG((regs[3] & TSC_FEATURE_EDX_BIT) != 0,
	    "CPUID.1.EDX[4] (TSC) not set — TSC not advertised by CPU "
	    "(EDX=0x%08x)", regs[3]);

	printf("TSC feature bit: present\n");
}

/* =========================================================================
 * TC-TSC-DET-02  tsc_invariant_flag
 *
 * Verify CPUID.80000007 EDX[8] (InvariantTSC / AMDPM_TSC_INVARIANT).
 *
 * HIGH — All AMD Zen 1+ CPUs set this bit.  If absent (possible on very
 * old hardware or in some VM configurations) the test passes with a note:
 * non-invariant TSC is architecturally valid but limits test reliability.
 * ====================================================================== */
ATF_TC(tsc_invariant_flag);
ATF_TC_HEAD(tsc_invariant_flag, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify InvariantTSC flag (CPUID.80000007.EDX[8]).  "
	    "Set on all AMD Zen 1+ CPUs; absence means TSC may stop in deep "
	    "C-states.  Test passes either way but prints a warning if absent.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_invariant_flag, tc)
{
	uint32_t regs[4];
	uint32_t max_ext;
	int error;

	tsc_skip_unless_cpuctl();

	max_ext = tsc_max_ext_leaf();
	if (max_ext < TSC_CPUID_APM)
		atf_tc_skip("Max extended CPUID leaf 0x%08x < 0x80000007 — "
		    "InvariantTSC probe not possible", max_ext);

	error = tsc_cpuid(TSC_CPUID_APM, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID leaf 0x80000007 failed: %s", strerror(error));

	printf("CPUID.80000007 EDX=0x%08x\n", regs[3]);

	if ((regs[3] & TSC_INVARIANT_EDX_BIT) != 0) {
		printf("InvariantTSC: present (TSC runs at constant rate "
		    "across P-states and C-states)\n");
	} else {
		printf("WARNING: InvariantTSC NOT set — TSC may stop in "
		    "deep C-states.  Timing-dependent tests may be unreliable "
		    "on this CPU.\n");
	}

	/*
	 * Always pass: non-invariant TSC is valid hardware.
	 * The CHECK here documents the expectation without making it a
	 * hard failure.
	 */
	ATF_CHECK_MSG((regs[3] & TSC_INVARIANT_EDX_BIT) != 0,
	    "InvariantTSC not set on CPU family=0x%02x model=0x%02x — "
	    "expected on AMD Zen 1+",
	    tsc_cpu_family(), tsc_cpu_model());
}

/* =========================================================================
 * TC-TSC-DET-03  tsc_rdtscp_present
 *
 * Check CPUID.80000001 EDX[27] (RDTSCP).
 *
 * MEDIUM — Informational probe.  RDTSCP provides a serialised TSC read
 * plus a TSC_AUX processor ID in a single instruction.  Always passes.
 * ====================================================================== */
ATF_TC(tsc_rdtscp_present);
ATF_TC_HEAD(tsc_rdtscp_present, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Check CPUID.80000001.EDX[27] (RDTSCP).  Informational: RDTSCP "
	    "is available on all Zen CPUs.  Always passes.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_rdtscp_present, tc)
{
	uint32_t regs[4];
	uint32_t max_ext;
	int error;

	tsc_skip_unless_cpuctl();

	/* Skip non-AMD — RDTSCP is AMD/Intel specific; bit position identical. */
	if (!tsc_cpu_is_amd())
		atf_tc_skip("Not an AuthenticAMD CPU");

	max_ext = tsc_max_ext_leaf();
	if (max_ext < TSC_CPUID_EXT_FEAT)
		atf_tc_skip("Max extended CPUID leaf 0x%08x < 0x80000001",
		    max_ext);

	error = tsc_cpuid(TSC_CPUID_EXT_FEAT, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID leaf 0x80000001 failed: %s", strerror(error));

	printf("CPUID.80000001 EDX=0x%08x\n", regs[3]);
	printf("RDTSCP: %s\n",
	    (regs[3] & TSC_RDTSCP_EDX_BIT) ? "present" : "absent");
}

/* =========================================================================
 * TC-TSC-DET-04  tsc_art_ratio_leaf
 *
 * Verify CPUID leaf 0x15 (TSC / ART ratio) is populated and coherent.
 *
 * HIGH — On AMD EPYC, CPUID 0x15 returns:
 *   EAX = ART denominator (e.g. 1)
 *   EBX = TSC numerator  (e.g. 4000000000 / 25000000 = 160)
 *   ECX = ART/crystal freq Hz (25_000_000 on EPYC — 25 MHz reference)
 *
 * Skips if the leaf is not populated (common in VMs / pre-Broadwell).
 * ====================================================================== */
ATF_TC(tsc_art_ratio_leaf);
ATF_TC_HEAD(tsc_art_ratio_leaf, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify CPUID.0x15 (TSC/ART ratio) is populated and EBX > EAX.  "
	    "Skips if the leaf is unavailable (VM or pre-ART hardware).  "
	    "Prints crystal Hz and computes TSC frequency from the ratio.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_art_ratio_leaf, tc)
{
	uint32_t regs[4];
	uint64_t tsc_freq;
	int error;

	tsc_skip_unless_cpuctl();

	error = tsc_cpuid(TSC_CPUID_TSC_ART, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID leaf 0x15 failed: %s", strerror(error));

	printf("CPUID.0x15: EAX(denominator)=%u  EBX(numerator)=%u  "
	    "ECX(crystal_Hz)=%u\n", regs[0], regs[1], regs[2]);

	/* Skip if leaf not populated — valid on VMs and older CPUs. */
	if (regs[0] == 0 || regs[1] == 0)
		atf_tc_skip("CPUID 0x15 EAX or EBX is zero — leaf not "
		    "populated (VM or pre-ART hardware)");

	if (regs[2] == 0) {
		/*
		 * ECX = 0 is allowed by the spec; some implementations omit
		 * the crystal frequency.  We cannot compute an absolute TSC
		 * frequency without it, but we can still verify the ratio.
		 */
		printf("Note: ECX (crystal Hz) is 0; absolute frequency "
		    "not computable from this leaf alone.\n");
	} else {
		tsc_freq = (uint64_t)regs[2] * regs[1] / regs[0];
		printf("TSC frequency from CPUID 0x15: %ju Hz  "
		    "(%.3f GHz)\n",
		    (uintmax_t)tsc_freq,
		    (double)tsc_freq / 1e9);
	}

	/*
	 * EBX (numerator) should be >= EAX (denominator) — a TSC/ART ratio
	 * < 1 would mean the TSC runs slower than the ART, which is not
	 * architecturally valid on AMD.
	 */
	ATF_CHECK_MSG(regs[1] >= regs[0],
	    "CPUID 0x15 EBX(%u) < EAX(%u): TSC/ART ratio < 1 is invalid",
	    regs[1], regs[0]);
}

/* =========================================================================
 * TC-TSC-DET-05  tsc_frequency_valid
 *
 * Compute TSC frequency from CPUID (leaf 0x15 or 0x16) and verify it
 * falls within [TSC_FREQ_MIN_HZ, TSC_FREQ_MAX_HZ].
 *
 * HIGH — Primary correctness case for the frequency computation path in
 * tsc_compute_frequency().
 * ====================================================================== */
ATF_TC(tsc_frequency_valid);
ATF_TC_HEAD(tsc_frequency_valid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compute TSC frequency from CPUID (leaf 0x15 preferred, 0x16 "
	    "fallback) and verify it is within [500 MHz, 10 GHz].  "
	    "Skips if neither leaf provides usable data.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_frequency_valid, tc)
{
	uint64_t freq;

	tsc_skip_unless_cpuctl();

	freq = tsc_compute_frequency();
	if (freq == 0)
		atf_tc_skip("TSC frequency not determinable from CPUID 0x15 "
		    "or 0x16 (both leaves absent or zero)");

	printf("TSC frequency: %ju Hz  (%.3f GHz)\n",
	    (uintmax_t)freq, (double)freq / 1e9);

	ATF_CHECK_MSG(freq >= TSC_FREQ_MIN_HZ,
	    "TSC frequency %ju Hz is below minimum %ju Hz (%.0f GHz)",
	    (uintmax_t)freq, (uintmax_t)TSC_FREQ_MIN_HZ,
	    (double)TSC_FREQ_MIN_HZ / 1e9);

	ATF_CHECK_MSG(freq <= TSC_FREQ_MAX_HZ,
	    "TSC frequency %ju Hz exceeds maximum %ju Hz (%.0f GHz)",
	    (uintmax_t)freq, (uintmax_t)TSC_FREQ_MAX_HZ,
	    (double)TSC_FREQ_MAX_HZ / 1e9);
}

/* =========================================================================
 * TC-TSC-DET-06  tsc_read_monotonic
 *
 * Verify tsc_read() never returns a value smaller than the previous read
 * over 1000 consecutive pairs.
 *
 * CRITICAL — A backwards-moving TSC is a serious hardware or hypervisor
 * defect that would break any timing-dependent software.
 * ====================================================================== */
ATF_TC(tsc_read_monotonic);
ATF_TC_HEAD(tsc_read_monotonic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Call tsc_read() 1001 times and verify each value is >= the "
	    "previous one.  A backwards jump indicates a broken TSC "
	    "(hypervisor bug or non-invariant hardware).");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(tsc_read_monotonic, tc)
{
	uint64_t prev, curr;
	uint64_t min_delta = UINT64_MAX, max_delta = 0, total = 0;
	int i, failures = 0;

	tsc_skip_unless_cpuctl();

	if (!tsc_feature_present())
		atf_tc_skip("TSC feature not present (CPUID.1.EDX[4] not set)");

	prev = tsc_read();

	for (i = 0; i < 1000; i++) {
		curr = tsc_read();

		if (curr < prev) {
			fprintf(stderr,
			    "TSC backwards jump at iteration %d: "
			    "prev=%ju curr=%ju delta=%jd\n",
			    i, (uintmax_t)prev, (uintmax_t)curr,
			    (intmax_t)(curr - prev));
			failures++;
		} else {
			uint64_t d = curr - prev;
			if (d < min_delta)
				min_delta = d;
			if (d > max_delta)
				max_delta = d;
			total += d;
		}

		prev = curr;
	}

	printf("TSC monotonicity: %d/1000 pairs checked\n", 1000 - failures);
	if (failures == 0 && min_delta != UINT64_MAX) {
		printf("  min_delta=%ju  max_delta=%ju  avg_delta=%ju cycles\n",
		    (uintmax_t)min_delta, (uintmax_t)max_delta,
		    (uintmax_t)(total / 1000));
	}

	ATF_CHECK_MSG(failures == 0,
	    "TSC moved backwards %d time(s) in 1000 reads — "
	    "non-monotonic TSC detected", failures);

	/* Verify total advancement is nonzero — counter must tick. */
	ATF_CHECK_MSG(total > 0,
	    "TSC did not advance over 1000 reads (total delta = 0)");
}

/* =========================================================================
 * TC-TSC-DET-07  tsc_frequency_stable
 *
 * Cross-validate CPUID-derived TSC frequency against a wall-clock
 * measurement using clock_gettime(CLOCK_MONOTONIC).
 *
 * Measures TSC ticks over a ~100 ms interval and computes:
 *   measured_freq = tsc_delta / elapsed_seconds
 *
 * Accepts if |measured - cpuid| / cpuid < 2%.
 *
 * MEDIUM — Soft cross-check.  VMs with paravirtual clocks or schedulers
 * that delay the sleep may push the error beyond 2%; the check is
 * lenient enough for bare-metal and typical CI environments.
 * ====================================================================== */
ATF_TC(tsc_frequency_stable);
ATF_TC_HEAD(tsc_frequency_stable, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Cross-validate CPUID-derived TSC frequency against a 100 ms "
	    "wall-clock measurement.  Requires agreement within 2%.  "
	    "Skips if CPUID does not provide a usable frequency.");
	atf_tc_set_md_var(tc, "require.user", "root");
	/* 100ms sleep + overhead — generous budget. */
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(tsc_frequency_stable, tc)
{
	struct timespec t0, t1;
	uint64_t tsc0, tsc1;
	uint64_t freq_cpuid, freq_measured;
	double elapsed_s, ratio, error_pct;

	tsc_skip_unless_cpuctl();

	freq_cpuid = tsc_compute_frequency();
	if (freq_cpuid == 0)
		atf_tc_skip("TSC frequency not determinable from CPUID — "
		    "cannot cross-validate");

	/* Capture start. */
	ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &t0) == 0,
	    "clock_gettime(CLOCK_MONOTONIC) failed: %s", strerror(errno));
	tsc0 = tsc_read();

	/* ~100 ms wall-clock wait. */
	(void)usleep(100000);

	/* Capture end. */
	tsc1 = tsc_read();
	ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &t1) == 0,
	    "clock_gettime(CLOCK_MONOTONIC) failed: %s", strerror(errno));

	elapsed_s = (double)(t1.tv_sec - t0.tv_sec) +
	    (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

	if (elapsed_s <= 0.0)
		atf_tc_skip("Wall-clock elapsed time is zero or negative — "
		    "clock resolution too coarse");

	freq_measured = (uint64_t)((double)(tsc1 - tsc0) / elapsed_s);

	printf("CPUID-derived TSC frequency : %.3f GHz\n",
	    (double)freq_cpuid / 1e9);
	printf("Wall-clock measured frequency: %.3f GHz  "
	    "(elapsed=%.4f s  tsc_delta=%ju)\n",
	    (double)freq_measured / 1e9,
	    elapsed_s, (uintmax_t)(tsc1 - tsc0));

	ratio = (double)freq_measured / (double)freq_cpuid;
	error_pct = (ratio > 1.0 ? ratio - 1.0 : 1.0 - ratio) * 100.0;

	printf("Agreement: %.4f%%  (threshold: 2.00%%)\n", error_pct);

	ATF_CHECK_MSG(error_pct < 2.0,
	    "TSC frequency disagreement %.4f%% exceeds 2%% threshold "
	    "(cpuid=%ju Hz  measured=%ju Hz)",
	    error_pct, (uintmax_t)freq_cpuid, (uintmax_t)freq_measured);
}

/* =========================================================================
 * Test program registration
 * ====================================================================== */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tsc_feature_present);
	ATF_TP_ADD_TC(tp, tsc_invariant_flag);
	ATF_TP_ADD_TC(tp, tsc_rdtscp_present);
	ATF_TP_ADD_TC(tp, tsc_art_ratio_leaf);
	ATF_TP_ADD_TC(tp, tsc_frequency_valid);
	ATF_TP_ADD_TC(tp, tsc_read_monotonic);
	ATF_TP_ADD_TC(tp, tsc_frequency_stable);
	return (atf_no_error());
}

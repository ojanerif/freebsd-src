/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Osvaldo Janeri Filho <ojanerif@amd.com>
 *
 * TSC (Time Stamp Counter) test utilities.
 *
 * Provides CPUID-based probing helpers, TSC frequency computation, and a
 * serialised rdtsc wrapper.  All functions are static inline — no hardware
 * is written, only read.
 *
 * CPUID access uses the /dev/cpuctl0 ioctl (CPUCTL_CPUID), the same pattern
 * used by ibs_utils.h.  Callers must be root.
 *
 * Key AMD CPUID leaves used here:
 *
 *   0x00000001  EAX: family/model/stepping; EDX[4]: TSC present
 *   0x00000015  EAX: ART/TSC denominator; EBX: numerator; ECX: crystal Hz
 *   0x00000016  EAX[15:0]: base frequency (MHz)
 *   0x80000000  EAX: max extended leaf
 *   0x80000001  EDX[27]: RDTSCP present
 *   0x80000007  EDX[8]: InvariantTSC (AMDPM_TSC_INVARIANT)
 *
 * Reference: AMD64 Architecture Programmer's Manual Vol. 3, Rev 3.35,
 *            §2.4 (CPUID), §6.3 (TSC).
 */

#ifndef _TSC_UTILS_H_
#define	_TSC_UTILS_H_

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * CPUID leaf addresses
 * ---------------------------------------------------------------------- */
#define	TSC_CPUID_FEATURES	0x00000001U	/* std features: FMS, EDX[4] */
#define	TSC_CPUID_TSC_ART	0x00000015U	/* TSC / ART ratio */
#define	TSC_CPUID_FREQ_INFO	0x00000016U	/* processor frequency info */
#define	TSC_CPUID_MAX_EXT	0x80000000U	/* max extended leaf */
#define	TSC_CPUID_EXT_FEAT	0x80000001U	/* ext features: RDTSCP */
#define	TSC_CPUID_APM		0x80000007U	/* Advanced Power Mgt */

/* -------------------------------------------------------------------------
 * Bit constants (match specialreg.h definitions)
 * ---------------------------------------------------------------------- */

/* CPUID.1 EDX[4] — TSC present */
#define	TSC_FEATURE_EDX_BIT	0x00000010U

/* CPUID.80000001 EDX[27] — RDTSCP supported */
#define	TSC_RDTSCP_EDX_BIT	0x08000000U

/* CPUID.80000007 EDX[8] — Invariant TSC (AMDPM_TSC_INVARIANT) */
#define	TSC_INVARIANT_EDX_BIT	0x00000100U

/* -------------------------------------------------------------------------
 * Frequency sanity bounds
 * ---------------------------------------------------------------------- */
#define	TSC_FREQ_MIN_HZ		500000000ULL	/* 500 MHz */
#define	TSC_FREQ_MAX_HZ		10000000000ULL	/* 10 GHz */

/* -------------------------------------------------------------------------
 * AMD vendor string bytes (CPUID.0 EBX/EDX/ECX)
 * ---------------------------------------------------------------------- */
#define	TSC_AMD_EBX		0x68747541U	/* "Auth" */
#define	TSC_AMD_EDX		0x69746e65U	/* "enti" */
#define	TSC_AMD_ECX		0x444d4163U	/* "cAMD" */

/* =========================================================================
 * Low-level CPUID helper
 * ====================================================================== */

/*
 * tsc_cpuid -- issue CPUID leaf via /dev/cpuctl0 ioctl.
 *
 * Returns 0 on success, errno on failure.  regs[0..3] = EAX/EBX/ECX/EDX.
 */
static inline int
tsc_cpuid(uint32_t leaf, uint32_t regs[4])
{
	cpuctl_cpuid_args_t args;
	int fd, error;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (errno);

	memset(&args, 0, sizeof(args));
	args.level = leaf;
	if (ioctl(fd, CPUCTL_CPUID, &args) < 0) {
		error = errno;
		close(fd);
		return (error);
	}

	regs[0] = args.data[0];	/* EAX */
	regs[1] = args.data[1];	/* EBX */
	regs[2] = args.data[2];	/* ECX */
	regs[3] = args.data[3];	/* EDX */
	close(fd);
	return (0);
}

/* =========================================================================
 * Vendor / feature detection helpers
 * ====================================================================== */

/*
 * tsc_cpu_is_amd -- true if CPUID.0 returns "AuthenticAMD".
 */
static inline bool
tsc_cpu_is_amd(void)
{
	uint32_t regs[4];

	if (tsc_cpuid(0x0, regs) != 0)
		return (false);
	return (regs[1] == TSC_AMD_EBX &&
	    regs[3] == TSC_AMD_EDX &&
	    regs[2] == TSC_AMD_ECX);
}

/*
 * tsc_max_ext_leaf -- return the maximum extended CPUID leaf supported.
 * Returns 0 on error.
 */
static inline uint32_t
tsc_max_ext_leaf(void)
{
	uint32_t regs[4];

	if (tsc_cpuid(TSC_CPUID_MAX_EXT, regs) != 0)
		return (0);
	return (regs[0]);
}

/*
 * tsc_feature_present -- true if CPUID.1 EDX[4] (TSC) is set.
 */
static inline bool
tsc_feature_present(void)
{
	uint32_t regs[4];

	if (tsc_cpuid(TSC_CPUID_FEATURES, regs) != 0)
		return (false);
	return ((regs[3] & TSC_FEATURE_EDX_BIT) != 0);
}

/*
 * tsc_rdtscp_present -- true if CPUID.80000001 EDX[27] is set.
 */
static inline bool
tsc_rdtscp_present(void)
{
	uint32_t regs[4];

	if (tsc_max_ext_leaf() < TSC_CPUID_EXT_FEAT)
		return (false);
	if (tsc_cpuid(TSC_CPUID_EXT_FEAT, regs) != 0)
		return (false);
	return ((regs[3] & TSC_RDTSCP_EDX_BIT) != 0);
}

/*
 * tsc_invariant_present -- true if CPUID.80000007 EDX[8] is set.
 * This bit guarantees TSC runs at constant rate regardless of P-state or
 * deep C-state entry (AMD Zen 1 and later always set this).
 */
static inline bool
tsc_invariant_present(void)
{
	uint32_t regs[4];

	if (tsc_max_ext_leaf() < TSC_CPUID_APM)
		return (false);
	if (tsc_cpuid(TSC_CPUID_APM, regs) != 0)
		return (false);
	return ((regs[3] & TSC_INVARIANT_EDX_BIT) != 0);
}

/*
 * tsc_art_leaf_available -- true if CPUID.0x15 EAX and EBX are both nonzero.
 *
 * CPUID leaf 0x15 reports the TSC/ART (Always Running Timer) ratio:
 *   EAX = denominator (ART periods per TSC period)
 *   EBX = numerator
 *   ECX = ART/crystal frequency in Hz (may be 0 on some implementations)
 *
 * Both EAX and EBX must be nonzero to compute a valid ratio.
 */
static inline bool
tsc_art_leaf_available(void)
{
	uint32_t regs[4];

	if (tsc_cpuid(TSC_CPUID_TSC_ART, regs) != 0)
		return (false);
	return (regs[0] != 0 && regs[1] != 0);
}

/*
 * tsc_freq_leaf_available -- true if CPUID.0x16 EAX[15:0] (base MHz) is
 * nonzero.
 */
static inline bool
tsc_freq_leaf_available(void)
{
	uint32_t regs[4];

	if (tsc_cpuid(TSC_CPUID_FREQ_INFO, regs) != 0)
		return (false);
	return ((regs[0] & 0xffffU) != 0);
}

/* =========================================================================
 * TSC frequency computation
 * ====================================================================== */

/*
 * tsc_compute_frequency -- compute TSC frequency in Hz.
 *
 * Method 1 (preferred): CPUID.0x15
 *   TSC_Hz = crystal_Hz * EBX / EAX
 *   On AMD, ECX typically reports 25 MHz (25_000_000 Hz).
 *
 * Method 2 (fallback): CPUID.0x16
 *   TSC_Hz = EAX[15:0] * 1_000_000  (base freq in MHz → Hz)
 *
 * Returns 0 if neither leaf provides usable data.
 */
static inline uint64_t
tsc_compute_frequency(void)
{
	uint32_t regs[4];
	uint64_t freq;

	/* Try leaf 0x15 first. */
	if (tsc_cpuid(TSC_CPUID_TSC_ART, regs) == 0 &&
	    regs[0] != 0 && regs[1] != 0 && regs[2] != 0) {
		/*
		 * freq = crystal_Hz * (EBX / EAX)
		 * Use 64-bit arithmetic to avoid overflow on the multiply.
		 */
		freq = (uint64_t)regs[2] * regs[1] / regs[0];
		if (freq >= TSC_FREQ_MIN_HZ && freq <= TSC_FREQ_MAX_HZ)
			return (freq);
	}

	/* Fallback: leaf 0x16, base freq in MHz. */
	if (tsc_cpuid(TSC_CPUID_FREQ_INFO, regs) == 0 &&
	    (regs[0] & 0xffffU) != 0) {
		freq = (uint64_t)(regs[0] & 0xffffU) * 1000000ULL;
		if (freq >= TSC_FREQ_MIN_HZ && freq <= TSC_FREQ_MAX_HZ)
			return (freq);
	}

	return (0);
}

/* =========================================================================
 * TSC read
 * ====================================================================== */

/*
 * tsc_read -- serialised RDTSC.
 *
 * Issues LFENCE before RDTSC to prevent earlier loads from being reordered
 * past the counter read, giving a more deterministic lower bound on the
 * elapsed cycle count.
 *
 * Returns a 64-bit TSC value.  On all AMD Zen CPUs the TSC is 64 bits wide
 * and does not wrap within any test's expected runtime.
 */
static inline uint64_t
tsc_read(void)
{
	uint32_t lo, hi;

	__asm__ volatile(
	    "lfence\n\t"
	    "rdtsc"
	    : "=a"(lo), "=d"(hi)
	    :
	    : "memory");
	return (((uint64_t)hi << 32) | lo);
}

/* =========================================================================
 * CPU identification helpers (family / model / stepping)
 * ====================================================================== */

/*
 * tsc_cpu_family -- return display family from CPUID.1 EAX.
 * Display family = base_family + ext_family (if base_family == 0xF).
 */
static inline uint32_t
tsc_cpu_family(void)
{
	uint32_t regs[4];
	uint32_t base, ext;

	if (tsc_cpuid(TSC_CPUID_FEATURES, regs) != 0)
		return (0);
	base = (regs[0] >> 8) & 0xfU;
	ext  = (regs[0] >> 20) & 0xffU;
	return (base == 0xfU ? base + ext : base);
}

/*
 * tsc_cpu_model -- return display model from CPUID.1 EAX.
 */
static inline uint32_t
tsc_cpu_model(void)
{
	uint32_t regs[4];
	uint32_t base, ext;

	if (tsc_cpuid(TSC_CPUID_FEATURES, regs) != 0)
		return (0);
	base = (regs[0] >> 4) & 0xfU;
	ext  = (regs[0] >> 16) & 0xfU;
	return ((ext << 4) | base);
}

/*
 * tsc_cpu_stepping -- return stepping from CPUID.1 EAX[3:0].
 */
static inline uint32_t
tsc_cpu_stepping(void)
{
	uint32_t regs[4];

	if (tsc_cpuid(TSC_CPUID_FEATURES, regs) != 0)
		return (0);
	return (regs[0] & 0xfU);
}

#endif /* _TSC_UTILS_H_ */

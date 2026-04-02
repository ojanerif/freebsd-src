/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * IBS L3MissOnly Filter Test
 *
 * This test validates the L3MissOnly filter behavior for AMD IBS
 * (Instruction-Based Sampling) on Zen 4+ processors.
 *
 * L3MissOnly is a Zen 4+ feature that filters IBS samples to only
 * capture instructions that experienced an L3 cache miss. This is
 * useful for identifying memory-bound code regions.
 *
 * IBS Fetch L3MissOnly:
 *   - Bit 59 of IBS Fetch Control (MSR 0xC0011030)
 *   - When set, only samples with L3 misses are recorded
 *
 * IBS Op L3MissOnly:
 *   - Bit 16 of IBS Op Control (MSR 0xC0011033)
 *   - When set, only Op samples with L3 misses are recorded
 *
 * Reference: Linux kernel ibs.c, AMD PPR documentation for Zen 4+
 */

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ibs_utils.h"

/*
 * L3MissOnly bit definitions for IBS control registers.
 * These are also defined in specialreg.h, but we define them
 * here for clarity and to match the Linux kernel conventions.
 *
 * IBS_FETCH_L3_MISS_ONLY: Bit 59 of IBS Fetch Control
 * IBS_OP_L3_MISS_ONLY: Bit 16 of IBS Op Control
 */
#define IBS_FETCH_L3_MISS_ONLY	0x0800000000000000ULL
#define IBS_OP_L3_MISS_ONLY	0x0000000000010000ULL

/*
 * Enable bits for IBS control registers.
 * We need to clear these when testing to avoid enabling sampling.
 */
#define IBS_FETCH_ENABLE_BIT	(1ULL << 2)	/* IBS_FETCH_EN */
#define IBS_OP_ENABLE_BIT	(1ULL << 17)	/* IBS_OP_EN */

/*
 * Helper: Check if the CPU supports L3MissOnly feature.
 * L3MissOnly is available on Zen 4+ (Family 19h) processors.
 * We check CPUID 0x8000001B bit 6 (IBS_ZEN4) to determine this.
 */
static inline bool
cpu_supports_l3miss_only(void)
{
	uint32_t regs[4];

	if (do_cpuid_ioctl(0x8000001B, regs) != 0)
		return (false);
	/* Bit 6 indicates Zen 4+ IBS extensions */
	return ((regs[0] & IBS_CPUID_ZEN4_IBS) != 0);
}

/*
 * Helper: Get the current CPU family to determine if it's Zen 4+.
 * Zen 4 is Family 19h (0x19).
 */
static inline uint32_t
get_cpu_family(void)
{
	uint32_t regs[4];
	uint32_t family;

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (0);
	family = ((regs[0] >> 8) & 0xf) + ((regs[0] >> 20) & 0xff);
	return (family);
}

/*
 * Test: ibs_l3miss_detect_zen4
 *
 * Detect Zen 4+ CPU with L3MissOnly capability.
 * This test verifies that we can correctly identify CPUs that
 * support the L3MissOnly filter feature.
 */
ATF_TC(ibs_l3miss_detect_zen4);
ATF_TC_HEAD(ibs_l3miss_detect_zen4, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Detect Zen 4+ CPU with L3MissOnly capability");
}

ATF_TC_BODY(ibs_l3miss_detect_zen4, tc)
{
	uint32_t family;
	uint32_t regs[4];
	bool has_zen4_ibs;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	family = get_cpu_family();
	ATF_CHECK(family > 0);

	/* Check for Zen 4+ IBS extensions via CPUID 0x8000001B */
	if (do_cpuid_ioctl(0x8000001B, regs) != 0)
		atf_tc_skip("Cannot read CPUID 0x8000001B");

	has_zen4_ibs = (regs[0] & IBS_CPUID_ZEN4_IBS) != 0;

	if (!has_zen4_ibs)
		atf_tc_skip("CPU does not support Zen 4+ IBS extensions "
		    "(L3MissOnly requires Family 19h+)");

	/* Verify we're on Zen 4+ (Family 19h) */
	ATF_CHECK_EQ(family, 0x19);
	ATF_CHECK(cpu_supports_l3miss_only());
}

/*
 * Test: ibs_l3miss_fetch_enable
 *
 * Enable L3MissOnly in IBS Fetch Control and verify.
 * This test writes the L3MissOnly bit to the Fetch Control MSR
 * and verifies it can be read back correctly.
 */
ATF_TC(ibs_l3miss_fetch_enable);
ATF_TC_HEAD(ibs_l3miss_fetch_enable, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Enable L3MissOnly in IBS Fetch Control and verify");
}

ATF_TC_BODY(ibs_l3miss_fetch_enable, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_supports_l3miss_only())
		atf_tc_skip("CPU does not support L3MissOnly feature");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Write L3MissOnly bit with enable bit cleared */
	written = (original & ~IBS_FETCH_ENABLE_BIT) | IBS_FETCH_L3_MISS_ONLY;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify L3MissOnly bit is set */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((readback & IBS_FETCH_L3_MISS_ONLY) != 0);

	/* Verify enable bit is still cleared */
	ATF_CHECK((readback & IBS_FETCH_ENABLE_BIT) == 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_l3miss_op_enable
 *
 * Enable L3MissOnly in IBS Op Control and verify.
 * This test writes the L3MissOnly bit to the Op Control MSR
 * and verifies it can be read back correctly.
 */
ATF_TC(ibs_l3miss_op_enable);
ATF_TC_HEAD(ibs_l3miss_op_enable, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Enable L3MissOnly in IBS Op Control and verify");
}

ATF_TC_BODY(ibs_l3miss_op_enable, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_supports_l3miss_only())
		atf_tc_skip("CPU does not support L3MissOnly feature");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Write L3MissOnly bit with enable bit cleared */
	written = (original & ~IBS_OP_ENABLE_BIT) | IBS_OP_L3_MISS_ONLY;
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify L3MissOnly bit is set */
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((readback & IBS_OP_L3_MISS_ONLY) != 0);

	/* Verify enable bit is still cleared */
	ATF_CHECK((readback & IBS_OP_ENABLE_BIT) == 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_l3miss_filter_behavior
 *
 * Test that L3MissOnly filter actually filters samples.
 * This test enables L3MissOnly in both Fetch and Op control,
 * then verifies the configuration is consistent.
 *
 * Note: Full behavioral testing would require enabling IBS sampling
 * and generating L3 misses, which is complex and hardware-dependent.
 * This test verifies the configuration aspect of the filter.
 */
ATF_TC(ibs_l3miss_filter_behavior);
ATF_TC_HEAD(ibs_l3miss_filter_behavior, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test L3MissOnly filter configuration in both Fetch and Op");
}

ATF_TC_BODY(ibs_l3miss_filter_behavior, tc)
{
	uint64_t fetch_orig, op_orig, fetch_written, op_written;
	uint64_t fetch_readback, op_readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_supports_l3miss_only())
		atf_tc_skip("CPU does not support L3MissOnly feature");

	/* Read original values */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &fetch_orig);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	error = read_msr(0, MSR_IBS_OP_CTL, &op_orig);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/*
	 * Configure both Fetch and Op with L3MissOnly enabled.
	 * Clear enable bits to prevent actual sampling.
	 * Set a small period value to ensure valid configuration.
	 */
	fetch_written = (fetch_orig & ~IBS_FETCH_ENABLE_BIT) |
	    IBS_FETCH_L3_MISS_ONLY | 0x0010;
	op_written = (op_orig & ~IBS_OP_ENABLE_BIT) |
	    IBS_OP_L3_MISS_ONLY | 0x0010;

	/* Write Fetch Control */
	error = write_msr(0, MSR_IBS_FETCH_CTL, fetch_written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Write Op Control */
	error = write_msr(0, MSR_IBS_OP_CTL, op_written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify Fetch */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &fetch_readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((fetch_readback & IBS_FETCH_L3_MISS_ONLY) != 0);

	/* Read back and verify Op */
	error = read_msr(0, MSR_IBS_OP_CTL, &op_readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((op_readback & IBS_OP_L3_MISS_ONLY) != 0);

	/*
	 * Verify that both L3MissOnly bits are set simultaneously.
	 * This confirms the filter can be configured for both
	 * Fetch and Op sampling paths.
	 */
	ATF_CHECK((fetch_readback & IBS_FETCH_L3_MISS_ONLY) != 0 &&
	    (op_readback & IBS_OP_L3_MISS_ONLY) != 0);

	/* Restore original values */
	error = write_msr(0, MSR_IBS_FETCH_CTL, fetch_orig);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_OP_CTL, op_orig);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_l3miss_disabled_on_older
 *
 * Verify L3MissOnly is not available on pre-Zen 4 CPUs.
 * This test checks that the L3MissOnly bit is not writable
 * on older CPU families.
 */
ATF_TC(ibs_l3miss_disabled_on_older);
ATF_TC_HEAD(ibs_l3miss_disabled_on_older, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify L3MissOnly is not available on pre-Zen 4 CPUs");
}

ATF_TC_BODY(ibs_l3miss_disabled_on_older, tc)
{
	uint32_t family;
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	family = get_cpu_family();
	ATF_CHECK(family > 0);

	/* Skip if we're on Zen 4+ (this test is for pre-Zen 4) */
	if (cpu_supports_l3miss_only())
		atf_tc_skip("CPU supports L3MissOnly (Zen 4+)");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/*
	 * Try to write L3MissOnly bit on pre-Zen 4.
	 * On older CPUs, this bit is reserved and should read back as 0.
	 */
	written = original | IBS_FETCH_L3_MISS_ONLY;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	if (error != 0) {
		/* Write failed, which is acceptable on pre-Zen 4 */
		ATF_CHECK(true);
	} else {
		/* Write succeeded, verify bit reads back as 0 */
		error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK((readback & IBS_FETCH_L3_MISS_ONLY) == 0);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	if (error != 0) {
		/* Best effort restore */
	}
}

/*
 * Register all test cases with the ATF test program.
 */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_l3miss_detect_zen4);
	ATF_TP_ADD_TC(tp, ibs_l3miss_fetch_enable);
	ATF_TP_ADD_TC(tp, ibs_l3miss_op_enable);
	ATF_TP_ADD_TC(tp, ibs_l3miss_filter_behavior);
	ATF_TP_ADD_TC(tp, ibs_l3miss_disabled_on_older);

	return (atf_no_error());
}

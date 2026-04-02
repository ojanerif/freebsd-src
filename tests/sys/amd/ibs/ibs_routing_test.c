/*-
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * IBS Routing and Configuration Test
 *
 * This test validates IBS event configuration and core PMU routing
 * for both IBS Fetch and IBS Op sampling mechanisms.
 *
 * IBS Fetch Control (MSR 0xc0011030):
 *   - Bit 48 (IBS_FETCH_EN): Enable fetch sampling
 *   - Bits [15:0]: Max count (period)
 *   - Bit 57 (IBS_RAND_EN): Random sampling enable (Zen 4+)
 *
 * IBS Op Control (MSR 0xc0011033):
 *   - Bit 17 (IBS_OP_EN): Enable op sampling
 *   - Bit 19 (IBS_CNT_CTL): Counter control (ring vs core)
 *   - Bits [15:0]: Max count (period)
 *
 * IBS Control (MSR 0xc001103a):
 *   - Global IBS control register
 *   - Bit 0: IBS Fetch Enable
 *   - Bit 1: IBS Op Enable
 *
 * Reference: Linux kernel ibs.c, AMD PPR documentation
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
 * IBS Fetch Control bit definitions
 */
#define IBS_FETCH_CTL_ENABLE		(1ULL << 48)	/* IBS_FETCH_EN */
#define IBS_FETCH_CTL_VALID		(1ULL << 49)	/* IBS_FETCH_VAL */
#define IBS_FETCH_CTL_COMPLETE		(1ULL << 50)	/* IBS_FETCH_COMP */
#define IBS_RAND_EN			(1ULL << 57)	/* Random enable (Zen 4+) */

/*
 * IBS Op Control bit definitions
 */
#define IBS_OP_CTL_ENABLE		(1ULL << 17)	/* IBS_OP_EN */
#define IBS_OP_CTL_VALID		(1ULL << 18)	/* IBS_OP_VAL */
#define IBS_CNT_CTL			(1ULL << 19)	/* Counter control */
#define IBS_OP_MAXCNT_EXT		(0x7FULL << 20)	/* Extended max count (Zen 4+) */

/*
 * IBS Global Control (MSR 0xc001103a) bit definitions
 */
#define IBSCTL_FETCH_EN			(1ULL << 0)	/* Global fetch enable */
#define IBSCTL_OP_EN			(1ULL << 1)	/* Global op enable */
#define IBSCTL_VALID_BITS		0x3ULL		/* Valid control bits */

/*
 * Helper: Clear enable bits from an IBS control value.
 * Used to safely modify control registers without enabling sampling.
 */
static inline uint64_t
ibs_fetch_clear_enable(uint64_t val)
{

	return (val & ~IBS_FETCH_CTL_ENABLE);
}

static inline uint64_t
ibs_op_clear_enable(uint64_t val)
{

	return (val & ~IBS_OP_CTL_ENABLE);
}

/*
 * Helper: Set the period (max count) field in IBS Fetch control.
 */
static inline uint64_t
ibs_fetch_set_period(uint64_t ctl, uint64_t period)
{

	return ((ctl & ~IBS_FETCH_MAXCNT) | (period & IBS_FETCH_MAXCNT));
}

/*
 * Helper: Set the period (max count) field in IBS Op control.
 */
static inline uint64_t
ibs_op_set_period(uint64_t ctl, uint64_t period)
{

	return ((ctl & ~IBS_OP_MAXCNT) | (period & IBS_OP_MAXCNT));
}

/*
 * Test: ibs_fetch_enable_disable
 *
 * Verify that we can enable and disable IBS Fetch sampling by
 * manipulating the IBS_FETCH_EN bit (bit 48) in MSR 0xc0011030.
 */
ATF_TC(ibs_fetch_enable_disable);
ATF_TC_HEAD(ibs_fetch_enable_disable, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch enable/disable via IBS_FETCH_EN bit");
}

ATF_TC_BODY(ibs_fetch_enable_disable, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Test enabling IBS Fetch */
	written = original | IBS_FETCH_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBS_FETCH_CTL_ENABLE, IBS_FETCH_CTL_ENABLE);

	/* Test disabling IBS Fetch */
	written = original & ~IBS_FETCH_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBS_FETCH_CTL_ENABLE, 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_enable_disable
 *
 * Verify that we can enable and disable IBS Op sampling by
 * manipulating the IBS_OP_EN bit (bit 17) in MSR 0xc0011033.
 */
ATF_TC(ibs_op_enable_disable);
ATF_TC_HEAD(ibs_op_enable_disable, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op enable/disable via IBS_OP_EN bit");
}

ATF_TC_BODY(ibs_op_enable_disable, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Test enabling IBS Op */
	written = original | IBS_OP_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBS_OP_CTL_ENABLE, IBS_OP_CTL_ENABLE);

	/* Test disabling IBS Op */
	written = original & ~IBS_OP_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBS_OP_CTL_ENABLE, 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_fetch_cnt_ctl
 *
 * Verify that we can configure the IBS Fetch count control bits.
 * The fetch count (bits 31:16) holds the current count value and
 * decrements until it reaches zero, at which point a sample is taken.
 * We verify that we can write and read back the count field.
 */
ATF_TC(ibs_fetch_cnt_ctl);
ATF_TC_HEAD(ibs_fetch_cnt_ctl, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch count control bits configuration");
}

ATF_TC_BODY(ibs_fetch_cnt_ctl, tc)
{
	uint64_t original, written, readback;
	int error;
	struct {
		uint64_t cnt;
		const char *desc;
	} test_cases[] = {
		{ 0x0001, "Minimum count value" },
		{ 0x0010, "Small count value" },
		{ 0x0100, "Medium count value" },
		{ 0x1000, "Large count value" },
	};
	size_t i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Clear enable bit to avoid actual sampling */
	original = ibs_fetch_clear_enable(original);

	for (i = 0; i < nitems(test_cases); i++) {
		/* Write count value (bits 31:16) */
		written = (test_cases[i].cnt << 16) & IBS_FETCH_CNT;
		written |= (original & ~IBS_FETCH_CNT);
		error = write_msr(0, MSR_IBS_FETCH_CTL, written);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Read back and verify */
		error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK_EQ((readback & IBS_FETCH_CNT) >> 16,
		    test_cases[i].cnt);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_cnt_ctl
 *
 * Verify that we can configure the IBS Op count control bits.
 * The cnt_ctl bit (bit 19) controls whether the counter counts
 * core cycles (0) or reference cycles (1). This is important
 * for routing IBS events to different PMU sources.
 */
ATF_TC(ibs_op_cnt_ctl);
ATF_TC_HEAD(ibs_op_cnt_ctl, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op cnt_ctl bit for core vs reference cycle counting");
}

ATF_TC_BODY(ibs_op_cnt_ctl, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Clear enable bit to avoid actual sampling */
	original = ibs_op_clear_enable(original);

	/* Test setting cnt_ctl bit (reference cycle counting) */
	written = original | IBS_CNT_CTL;
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBS_CNT_CTL, IBS_CNT_CTL);

	/* Test clearing cnt_ctl bit (core cycle counting) */
	written = original & ~IBS_CNT_CTL;
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBS_CNT_CTL, 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_ibsctl_global
 *
 * Verify that we can read and write the global IBS control register
 * (MSR 0xc001103a). This register controls global IBS enable bits
 * for both Fetch and Op sampling.
 *
 * Note: This MSR may require elevated privileges and may not be
 * writable on all systems.
 */
ATF_TC(ibs_ibsctl_global);
ATF_TC_HEAD(ibs_ibsctl_global, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify global IBS control register (MSR 0xc001103a)");
}

ATF_TC_BODY(ibs_ibsctl_global, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_AMD64_IBSCTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_AMD64_IBSCTL: %s",
		    strerror(error));

	/* Test setting fetch enable bit */
	written = original | IBSCTL_FETCH_EN;
	error = write_msr(0, MSR_AMD64_IBSCTL, written);
	if (error != 0) {
		/* May require root or specific capabilities */
		atf_tc_skip("Cannot write MSR_AMD64_IBSCTL: %s",
		    strerror(error));
	}

	error = read_msr(0, MSR_AMD64_IBSCTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBSCTL_FETCH_EN, IBSCTL_FETCH_EN);

	/* Test setting op enable bit */
	written = original | IBSCTL_OP_EN;
	error = write_msr(0, MSR_AMD64_IBSCTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_AMD64_IBSCTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBSCTL_OP_EN, IBSCTL_OP_EN);

	/* Test clearing both bits */
	written = original & ~IBSCTL_VALID_BITS;
	error = write_msr(0, MSR_AMD64_IBSCTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_AMD64_IBSCTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(readback & IBSCTL_VALID_BITS, 0);

	/* Restore original value */
	error = write_msr(0, MSR_AMD64_IBSCTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_random_enable
 *
 * Verify that we can configure the random sampling enable bit
 * (IBS_RAND_EN, bit 57) in IBS Fetch Control. This is a Zen 4+
 * feature that enables random sampling instead of periodic sampling.
 *
 * On pre-Zen 4 CPUs, this bit may be reserved or have no effect.
 */
ATF_TC(ibs_random_enable);
ATF_TC_HEAD(ibs_random_enable, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS random sampling enable bit (Zen 4+ feature)");
}

ATF_TC_BODY(ibs_random_enable, tc)
{
	uint64_t original, written, readback;
	int error;
	bool is_zen4;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	is_zen4 = cpu_is_zen4();

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Clear enable bit to avoid actual sampling */
	original = ibs_fetch_clear_enable(original);

	/* Test setting random enable bit */
	written = original | IBS_RAND_EN;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* On Zen 4+, verify the bit is set; on older CPUs, it may be ignored */
	if (is_zen4) {
		ATF_CHECK_EQ(readback & IBS_RAND_EN, IBS_RAND_EN);
	} else {
		/* Pre-Zen 4: bit may be reserved, just verify write succeeded */
		atf_tc_expect_pass();
	}

	/* Test clearing random enable bit */
	written = original & ~IBS_RAND_EN;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);

	if (is_zen4) {
		ATF_CHECK_EQ(readback & IBS_RAND_EN, 0);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Register all test cases
 */
ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, ibs_fetch_enable_disable);
	ATF_TP_ADD_TC(tp, ibs_op_enable_disable);
	ATF_TP_ADD_TC(tp, ibs_fetch_cnt_ctl);
	ATF_TP_ADD_TC(tp, ibs_op_cnt_ctl);
	ATF_TP_ADD_TC(tp, ibs_ibsctl_global);
	ATF_TP_ADD_TC(tp, ibs_random_enable);

	return (atf_no_error());
}

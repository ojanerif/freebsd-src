/*-
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * IBS Period Validation Test
 *
 * This test validates the period (max count) encoding and behavior
 * for both IBS Fetch and IBS Op sampling mechanisms.
 *
 * IBS Fetch period:
 *   - Encoded in bits [15:0] of IBSFETCHCTL (MSR 0xc0011030)
 *   - The actual period is (maxcnt << 4), i.e., maxcnt * 16
 *   - Minimum period: 0x10 (16 cycles, maxcnt = 1)
 *   - Maximum period: 0xFFFF0 (maxcnt = 0xFFFF)
 *
 * IBS Op period:
 *   - Encoded in bits [15:0] of IBSOPCTL (MSR 0xc0011033)
 *   - Extended count in bits [26:20] for Zen4+ (IBS_OP_MAXCNT_EXT)
 *   - The actual period is (maxcnt << 4), i.e., maxcnt * 16
 *   - Minimum period: 0x10 (16 cycles, maxcnt = 1)
 *   - Maximum period: 0xFFFF0 (maxcnt = 0xFFFF)
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
 * Period encoding constants
 *
 * The IBS period is stored in the MAXCNT field. The actual sampling
 * interval is (MAXCNT << 4) clock cycles.
 *
 * Per AMD documentation:
 * - MAXCNT = 0 is invalid (no sampling occurs)
 * - MAXCNT = 1 gives minimum period of 16 cycles
 * - MAXCNT = 0xFFFF gives maximum period of 0xFFFF0 cycles
 *
 * The period field occupies bits [15:0] for both Fetch and Op control
 * registers. For IBS Op on Zen4+, there's an extended field in bits
 * [26:20] that allows even larger periods.
 */
#define IBS_PERIOD_MIN		0x0001	/* Minimum valid maxcnt value */
#define IBS_PERIOD_MAX		0xFFFF	/* Maximum valid maxcnt value */
#define IBS_PERIOD_SHIFT	4	/* Period = maxcnt << 4 */
#define IBS_PERIOD_MIN_ACTUAL	(IBS_PERIOD_MIN << IBS_PERIOD_SHIFT)
#define IBS_PERIOD_MAX_ACTUAL	(IBS_PERIOD_MAX << IBS_PERIOD_SHIFT)

/*
 * Mask for the period (max count) field in IBS control registers.
 * Both Fetch and Op use the same bit positions for the base count.
 */
#define IBS_MAXCNT_MASK		0x000000000000FFFFULL

/*
 * Enable bit masks for IBS control registers.
 * We clear these when testing to avoid actually enabling sampling.
 */
#define IBS_FETCH_ENABLE_BIT	(1ULL << 2)	/* IBS_FETCH_EN */
#define IBS_OP_ENABLE_BIT	(1ULL << 17)	/* IBS_OP_EN */

/*
 * Helper: Extract the maxcnt (period) field from an IBS control value.
 */
static inline uint64_t
ibs_get_maxcnt(uint64_t ctl_val)
{
	return (ctl_val & IBS_MAXCNT_MASK);
}

/*
 * Helper: Set the maxcnt (period) field in an IBS control value.
 */
static inline uint64_t
ibs_set_maxcnt(uint64_t ctl_val, uint64_t maxcnt)
{
	return ((ctl_val & ~IBS_MAXCNT_MASK) | (maxcnt & IBS_MAXCNT_MASK));
}

/*
 * Helper: Calculate actual period from maxcnt value.
 * The actual sampling period is maxcnt * 16 (maxcnt << 4).
 */
static inline uint64_t
ibs_maxcnt_to_period(uint64_t maxcnt)
{
	return (maxcnt << IBS_PERIOD_SHIFT);
}

/*
 * Helper: Calculate maxcnt from desired period.
 * Returns the maxcnt value that would produce the given period.
 */


/*
 * Test: ibs_fetch_period_basic
 *
 * Verify that we can write and read back various valid period values
 * for the IBS Fetch Control register.
 */
ATF_TC(ibs_fetch_period_basic);
ATF_TC_HEAD(ibs_fetch_period_basic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch period basic read/write with valid values");
}

ATF_TC_BODY(ibs_fetch_period_basic, tc)
{
	uint64_t original, written, readback;
	int error;
	struct {
		uint64_t maxcnt;
		const char *desc;
	} test_cases[] = {
		{ 0x0010, "Small period (256 cycles)" },
		{ 0x0100, "Medium period (4096 cycles)" },
		{ 0x1000, "Large period (65536 cycles)" },
		{ 0x8000, "Very large period (524288 cycles)" },
	};
	size_t i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	for (i = 0; i < nitems(test_cases); i++) {
		/* Write period with enable bit cleared */
		written = ibs_set_maxcnt(0, test_cases[i].maxcnt);
		error = write_msr(0, MSR_IBS_FETCH_CTL, written);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Read back and verify maxcnt field */
		error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK_EQ(ibs_get_maxcnt(readback), test_cases[i].maxcnt);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_fetch_period_min
 *
 * Verify the minimum period constraint for IBS Fetch.
 * The minimum valid maxcnt is 1, giving a period of 16 cycles.
 */
ATF_TC(ibs_fetch_period_min);
ATF_TC_HEAD(ibs_fetch_period_min, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch minimum period constraint (maxcnt=1, period=16)");
}

ATF_TC_BODY(ibs_fetch_period_min, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Write minimum valid maxcnt (1) */
	written = ibs_set_maxcnt(0, IBS_PERIOD_MIN);
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), IBS_PERIOD_MIN);

	/* Verify the actual period calculation */
	ATF_CHECK_EQ(ibs_maxcnt_to_period(ibs_get_maxcnt(readback)),
	    IBS_PERIOD_MIN_ACTUAL);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_fetch_period_max
 *
 * Verify the maximum period value for IBS Fetch.
 * The maximum valid maxcnt is 0xFFFF, giving a period of 0xFFFF0 cycles.
 */
ATF_TC(ibs_fetch_period_max);
ATF_TC_HEAD(ibs_fetch_period_max, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch maximum period value (maxcnt=0xFFFF)");
}

ATF_TC_BODY(ibs_fetch_period_max, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Write maximum valid maxcnt (0xFFFF) */
	written = ibs_set_maxcnt(0, IBS_PERIOD_MAX);
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), IBS_PERIOD_MAX);

	/* Verify the actual period calculation */
	ATF_CHECK_EQ(ibs_maxcnt_to_period(ibs_get_maxcnt(readback)),
	    IBS_PERIOD_MAX_ACTUAL);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_fetch_period_zero
 *
 * Verify that a zero period (maxcnt=0) is handled correctly.
 * Per AMD documentation, maxcnt=0 means no sampling occurs.
 */
ATF_TC(ibs_fetch_period_zero);
ATF_TC_HEAD(ibs_fetch_period_zero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch zero period handling (maxcnt=0)");
}

ATF_TC_BODY(ibs_fetch_period_zero, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Write zero maxcnt - this should be accepted but means no sampling */
	written = ibs_set_maxcnt(0, 0);
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify maxcnt is zero */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_period_basic
 *
 * Verify that we can write and read back various valid period values
 * for the IBS Op Control register.
 */
ATF_TC(ibs_op_period_basic);
ATF_TC_HEAD(ibs_op_period_basic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op period basic read/write with valid values");
}

ATF_TC_BODY(ibs_op_period_basic, tc)
{
	uint64_t original, written, readback;
	int error;
	struct {
		uint64_t maxcnt;
		const char *desc;
	} test_cases[] = {
		{ 0x0010, "Small period (256 cycles)" },
		{ 0x0100, "Medium period (4096 cycles)" },
		{ 0x1000, "Large period (65536 cycles)" },
		{ 0x8000, "Very large period (524288 cycles)" },
	};
	size_t i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	for (i = 0; i < nitems(test_cases); i++) {
		/* Write period with enable bit cleared */
		written = ibs_set_maxcnt(0, test_cases[i].maxcnt);
		error = write_msr(0, MSR_IBS_OP_CTL, written);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Read back and verify maxcnt field */
		error = read_msr(0, MSR_IBS_OP_CTL, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK_EQ(ibs_get_maxcnt(readback), test_cases[i].maxcnt);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_period_min
 *
 * Verify the minimum period constraint for IBS Op.
 * The minimum valid maxcnt is 1, giving a period of 16 cycles.
 */
ATF_TC(ibs_op_period_min);
ATF_TC_HEAD(ibs_op_period_min, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op minimum period constraint (maxcnt=1, period=16)");
}

ATF_TC_BODY(ibs_op_period_min, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Write minimum valid maxcnt (1) */
	written = ibs_set_maxcnt(0, IBS_PERIOD_MIN);
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), IBS_PERIOD_MIN);

	/* Verify the actual period calculation */
	ATF_CHECK_EQ(ibs_maxcnt_to_period(ibs_get_maxcnt(readback)),
	    IBS_PERIOD_MIN_ACTUAL);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_period_max
 *
 * Verify the maximum period value for IBS Op.
 * The maximum valid maxcnt is 0xFFFF, giving a period of 0xFFFF0 cycles.
 */
ATF_TC(ibs_op_period_max);
ATF_TC_HEAD(ibs_op_period_max, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op maximum period value (maxcnt=0xFFFF)");
}

ATF_TC_BODY(ibs_op_period_max, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Write maximum valid maxcnt (0xFFFF) */
	written = ibs_set_maxcnt(0, IBS_PERIOD_MAX);
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), IBS_PERIOD_MAX);

	/* Verify the actual period calculation */
	ATF_CHECK_EQ(ibs_maxcnt_to_period(ibs_get_maxcnt(readback)),
	    IBS_PERIOD_MAX_ACTUAL);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_period_zero
 *
 * Verify that a zero period (maxcnt=0) is handled correctly for IBS Op.
 * Per AMD documentation, maxcnt=0 means no sampling occurs.
 */
ATF_TC(ibs_op_period_zero);
ATF_TC_HEAD(ibs_op_period_zero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op zero period handling (maxcnt=0)");
}

ATF_TC_BODY(ibs_op_period_zero, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Write zero maxcnt - this should be accepted but means no sampling */
	written = ibs_set_maxcnt(0, 0);
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify maxcnt is zero */
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), 0);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_fetch_period_rollover
 *
 * Test period rollover behavior for IBS Fetch.
 * When the counter reaches zero, it should reload from the maxcnt value.
 * We verify this by writing a value, then checking the counter decrements.
 *
 * Note: This test verifies the encoding relationship between maxcnt
 * and the actual period, not the hardware counter behavior (which
 * would require enabling IBS and waiting for samples).
 */
ATF_TC(ibs_fetch_period_rollover);
ATF_TC_HEAD(ibs_fetch_period_rollover, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch period encoding and rollover relationship");
}

ATF_TC_BODY(ibs_fetch_period_rollover, tc)
{
	uint64_t original, written, readback;
	uint64_t maxcnt, expected_period;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/*
	 * Test the period encoding relationship:
	 * period = maxcnt << 4
	 *
	 * Write a known maxcnt and verify the period calculation is correct.
	 */
	maxcnt = 0x100;  /* 256 */
	written = ibs_set_maxcnt(0, maxcnt);
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify the maxcnt was written correctly */
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), maxcnt);

	/* Verify period calculation: 256 << 4 = 4096 */
	expected_period = ibs_maxcnt_to_period(maxcnt);
	ATF_CHECK_EQ(expected_period, 4096ULL);

	/* Test boundary: maxcnt + 1 should give period + 16 */
	maxcnt = 0x101;
	expected_period = ibs_maxcnt_to_period(maxcnt);
	ATF_CHECK_EQ(expected_period, 4112ULL);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_period_rollover
 *
 * Test period rollover behavior for IBS Op.
 * Similar to the fetch test, we verify the encoding relationship.
 */
ATF_TC(ibs_op_period_rollover);
ATF_TC_HEAD(ibs_op_period_rollover, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op period encoding and rollover relationship");
}

ATF_TC_BODY(ibs_op_period_rollover, tc)
{
	uint64_t original, written, readback;
	uint64_t maxcnt, expected_period;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/*
	 * Test the period encoding relationship:
	 * period = maxcnt << 4
	 *
	 * Write a known maxcnt and verify the period calculation is correct.
	 */
	maxcnt = 0x200;  /* 512 */
	written = ibs_set_maxcnt(0, maxcnt);
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify the maxcnt was written correctly */
	ATF_CHECK_EQ(ibs_get_maxcnt(readback), maxcnt);

	/* Verify period calculation: 512 << 4 = 8192 */
	expected_period = ibs_maxcnt_to_period(maxcnt);
	ATF_CHECK_EQ(expected_period, 8192ULL);

	/* Test boundary: maxcnt + 1 should give period + 16 */
	maxcnt = 0x201;
	expected_period = ibs_maxcnt_to_period(maxcnt);
	ATF_CHECK_EQ(expected_period, 8208ULL);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Register all test cases with the ATF test program.
 */
ATF_TP_ADD_TCS(tp)
{
	/* IBS Fetch period tests */
	ATF_TP_ADD_TC(tp, ibs_fetch_period_basic);
	ATF_TP_ADD_TC(tp, ibs_fetch_period_min);
	ATF_TP_ADD_TC(tp, ibs_fetch_period_max);
	ATF_TP_ADD_TC(tp, ibs_fetch_period_zero);
	ATF_TP_ADD_TC(tp, ibs_fetch_period_rollover);

	/* IBS Op period tests */
	ATF_TP_ADD_TC(tp, ibs_op_period_basic);
	ATF_TP_ADD_TC(tp, ibs_op_period_min);
	ATF_TP_ADD_TC(tp, ibs_op_period_max);
	ATF_TP_ADD_TC(tp, ibs_op_period_zero);
	ATF_TP_ADD_TC(tp, ibs_op_period_rollover);

	return (atf_no_error());
}

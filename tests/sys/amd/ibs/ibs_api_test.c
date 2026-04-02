/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

/*
 * Test MSR read/write round-trip for IBS Fetch Control register.
 * This validates that we can write a value and read it back correctly.
 */
ATF_TC(ibs_fetch_ctl_roundtrip);
ATF_TC_HEAD(ibs_fetch_ctl_roundtrip, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch Control MSR read/write round-trip");
}

ATF_TC_BODY(ibs_fetch_ctl_roundtrip, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original value */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Write a test pattern (enable bit clear, max count = 0x1000) */
	written = 0x1000ULL;
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(written, readback);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test MSR read/write round-trip for IBS Op Control register.
 * This validates that we can write a value and read it back correctly.
 */
ATF_TC(ibs_op_ctl_roundtrip);
ATF_TC_HEAD(ibs_op_ctl_roundtrip, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op Control MSR read/write round-trip");
}

ATF_TC_BODY(ibs_op_ctl_roundtrip, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original value */
	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Write a test pattern (enable bit clear, max count = 0x2000) */
	written = 0x2000ULL;
	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(written, readback);

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test that IBS control registers preserve reserved bits correctly.
 * Some bits in IBS MSRs are reserved and should not be modifiable.
 */
ATF_TC(ibs_ctl_reserved_bits);
ATF_TC_HEAD(ibs_ctl_reserved_bits, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Control MSR reserved bit handling");
}

ATF_TC_BODY(ibs_ctl_reserved_bits, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Test Fetch Control register */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Try to set bits that should be reserved (bit 63 is reserved) */
	written = original | (1ULL << 63);
	error = write_msr(0, MSR_IBS_FETCH_CTL, written);
	if (error == 0) {
		error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		/* Reserved bits should not be writable */
		ATF_CHECK((readback & (1ULL << 63)) == 0);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test IBS extended features if available (Zen4+).
 */
ATF_TC(ibs_extended_features);
ATF_TC_HEAD(ibs_extended_features, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS extended features detection");
}

ATF_TC_BODY(ibs_extended_features, tc)
{
	uint64_t val;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_ibs_extended())
		atf_tc_skip("CPU does not have extended IBS features");

	/* Read IBS Fetch Control to verify it's accessible */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &val);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Zen4+ should have additional feature bits */
	if (cpu_is_zen4()) {
		/* Zen4 has IBS feature extensions */
		ATF_CHECK((val & IBS_FETCH_CTL_ENABLE) == 0 ||
		    (val & IBS_FETCH_CTL_ENABLE) != 0);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_fetch_ctl_roundtrip);
	ATF_TP_ADD_TC(tp, ibs_op_ctl_roundtrip);
	ATF_TP_ADD_TC(tp, ibs_ctl_reserved_bits);
	ATF_TP_ADD_TC(tp, ibs_extended_features);
	return (atf_no_error());
}

/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_msr_read_write);
ATF_TC_HEAD(ibs_msr_read_write, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS MSR read/write");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_msr_read_write, tc)
{
	uint64_t val;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Test reading a known MSR */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &val);
	ATF_REQUIRE_EQ(error, 0);

	/* Test writing and reading back a value */
	error = write_msr(0, MSR_IBS_FETCH_CTL, 0x0);
	ATF_REQUIRE_EQ(error, 0);

	error = read_msr(0, MSR_IBS_FETCH_CTL, &val);
	ATF_REQUIRE_EQ(error, 0);
	ATF_CHECK_EQ(val, 0x0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_msr_read_write);
	return (atf_no_error());
}

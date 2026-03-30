/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_stress_test);
ATF_TC_HEAD(ibs_stress_test, tc)
{
	atf_tc_set_md_var(tc, "descr", "Run a stress test on IBS");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_stress_test, tc)
{
	uint64_t op_ctl;
	int error;
	volatile int i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Enable IBS op sampling */
	op_ctl = 1ULL << 17; /* Enable IBS op */
	error = write_msr(0, MSR_IBS_OP_CTL, op_ctl);
	ATF_REQUIRE_EQ(error, 0);

	/* Run a busy loop */
	for (i = 0; i < 1000000; i++) {
		__asm__ __volatile__("nop");
	}

	/* Disable IBS op sampling */
	error = write_msr(0, MSR_IBS_OP_CTL, 0);
	ATF_REQUIRE_EQ(error, 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_stress_test);
	return (atf_no_error());
}

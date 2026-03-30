/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_interrupt_test);
ATF_TC_HEAD(ibs_interrupt_test, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS interrupt generation");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_interrupt_test, tc)
{
	uint64_t fetch_ctl;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Enable IBS fetch sampling */
	fetch_ctl = 1ULL << 17; /* Enable IBS fetch */
	error = write_msr(0, MSR_IBS_FETCH_CTL, fetch_ctl);
	ATF_REQUIRE_EQ(error, 0);

	/*
	 * TODO: Add a mechanism to verify interrupt was received.
	 * This is non-trivial from userspace. For now, we just
	 * enable it and wait a moment.
	 */
	usleep(1000);

	/* Disable IBS fetch sampling */
	error = write_msr(0, MSR_IBS_FETCH_CTL, 0);
	ATF_REQUIRE_EQ(error, 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_interrupt_test);
	return (atf_no_error());
}

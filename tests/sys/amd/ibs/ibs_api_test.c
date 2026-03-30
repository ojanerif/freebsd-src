/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_api_test);
ATF_TC_HEAD(ibs_api_test, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify future IBS kernel API");
}

ATF_TC_BODY(ibs_api_test, tc)
{
	atf_tc_skip("No kernel API for IBS yet");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_api_test);
	return (atf_no_error());
}

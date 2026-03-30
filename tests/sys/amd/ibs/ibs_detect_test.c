/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_detect);
ATF_TC_HEAD(ibs_detect, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS feature detection");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_detect, tc)
{
	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ATF_CHECK(cpu_supports_ibs());
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_detect);
	return (atf_no_error());
}

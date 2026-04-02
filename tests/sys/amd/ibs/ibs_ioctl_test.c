/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 * All rights reserved.
 */

#include <atf-c.h>

#include "ibs_utils.h"

/*
 * IBS ioctl API tests - currently disabled due to kernel API not implemented
 *
 * These tests would validate the hwpmc syscall interface for IBS-specific
 * operations, but the kernel-side ioctl handlers are not yet implemented.
 * The tests are kept for future implementation when the kernel API is ready.
 */

ATF_TC(ibs_ioctl_not_implemented);
ATF_TC_HEAD(ibs_ioctl_not_implemented, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "IBS ioctl API tests - kernel support not yet implemented");
}

ATF_TC_BODY(ibs_ioctl_not_implemented, tc)
{
	/* Skip all ioctl tests until kernel support is implemented */
	atf_tc_skip("IBS ioctl API not yet implemented in kernel");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_ioctl_not_implemented);
	return (atf_no_error());
}
/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Sponsored by: Advanced Micro Devices, Inc.
 * Author: davi.chavesazevedo@amd.com
 *
 * Tests for hwpmc/libpmc IBS class discovery and visibility.
 */

#include <sys/param.h>

#include <atf-c.h>

#include "ibs_hwpmc_utils.h"

ATF_TC(ibs_hwpmc_info_class_present);
ATF_TC_HEAD(ibs_hwpmc_info_class_present, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify hwpmc exports PMC_CLASS_IBS with the expected class "
	    "shape");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_info_class_present, tc)
{
	const struct pmc_classinfo *classinfo;
	uint32_t expected_caps;

	ibs_test_skip_unless_hwpmc_ibs();
	expected_caps = PMC_CAP_INTERRUPT | PMC_CAP_SYSTEM | PMC_CAP_EDGE |
	    PMC_CAP_QUALIFIER | PMC_CAP_PRECISE;

	classinfo = ibs_test_find_classinfo();
	ATF_REQUIRE_MSG(classinfo != NULL,
	    "PMC_CLASS_IBS was not reported by pmc_cpuinfo()");

	ATF_CHECK_EQ(classinfo->pm_num, 2);
	ATF_CHECK_EQ(classinfo->pm_width, 0);
	ATF_CHECK_MSG((classinfo->pm_caps & expected_caps) == expected_caps,
	    "IBS class caps %#x missing required caps %#x", classinfo->pm_caps,
	    expected_caps);
}

ATF_TC(ibs_hwpmc_info_class_name_visible);
ATF_TC_HEAD(ibs_hwpmc_info_class_name_visible, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify libpmc resolves the IBS class name");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_info_class_name_visible, tc)
{
	const char *name;

	ibs_test_skip_unless_hwpmc_ibs();

	name = pmc_name_of_class(PMC_CLASS_IBS);
	ATF_REQUIRE_MSG(name != NULL,
	    "pmc_name_of_class(PMC_CLASS_IBS) returned NULL");
	ATF_CHECK_STREQ(name, "IBS");
}

ATF_TC(ibs_hwpmc_info_event_names_visible);
ATF_TC_HEAD(ibs_hwpmc_info_event_names_visible, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify libpmc exposes IBS event names FETCH and OP");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_info_event_names_visible, tc)
{
	ibs_test_skip_unless_hwpmc_ibs();

	ATF_REQUIRE_MSG(ibs_test_event_name_visible("FETCH"),
	    "FETCH was not visible in PMC_CLASS_IBS event names");
	ATF_REQUIRE_MSG(ibs_test_event_name_visible("OP"),
	    "OP was not visible in PMC_CLASS_IBS event names");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_hwpmc_info_class_present);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_info_class_name_visible);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_info_event_names_visible);
	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Sponsored by: Advanced Micro Devices, Inc.
 * Author: davi.chavesazevedo@amd.com
 */

#include <sys/param.h>

#include <atf-c.h>

#include "ibs_hwpmc_utils.h"

ATF_TC(ibs_hwpmc_fetch_caps_and_width);
ATF_TC_HEAD(ibs_hwpmc_fetch_caps_and_width, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify pmc_capabilities() and pmc_width() for IBS Fetch");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_fetch_caps_and_width, tc)
{
	pmc_id_t pmcid;
	uint32_t caps, width;
	uint32_t expected_caps;
	int error;

	pmcid = PMC_ID_INVALID;
	caps = 0;
	width = UINT32_MAX;
	expected_caps = PMC_CAP_INTERRUPT | PMC_CAP_SYSTEM | PMC_CAP_EDGE |
	    PMC_CAP_QUALIFIER | PMC_CAP_PRECISE;
	ibs_test_skip_unless_hwpmc_ibs();

	error = pmc_allocate("IBS-FETCH", PMC_MODE_SS, 0, 0, &pmcid, 65536);
	ATF_REQUIRE_MSG(error == 0, "pmc_allocate(IBS-FETCH) failed: %s",
	    strerror(errno));

	if (pmc_capabilities(pmcid, &caps) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_capabilities() failed: %s", strerror(errno));
	}
	if (pmc_width(pmcid, &width) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_width() failed: %s", strerror(errno));
	}

	ATF_CHECK_MSG((caps & expected_caps) == expected_caps,
	    "IBS Fetch caps %#x missing required caps %#x", caps, expected_caps);
	ATF_CHECK_EQ(width, 0);

	amd_test_release_pmc(pmcid);
}

ATF_TC(ibs_hwpmc_pmcinfo_rows_present);
ATF_TC_HEAD(ibs_hwpmc_pmcinfo_rows_present, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify pmc_pmcinfo() exposes IBS-FETCH and IBS-OP rows");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_pmcinfo_rows_present, tc)
{
	size_t fetch_rows, op_rows;

	ibs_test_skip_unless_hwpmc_ibs();

	fetch_rows = amd_test_count_pmc_rows_with_prefix(0, "IBS-FETCH");
	op_rows = amd_test_count_pmc_rows_with_prefix(0, "IBS-OP");

	ATF_REQUIRE_MSG(fetch_rows == 1,
	    "Expected exactly one IBS-FETCH row, found %zu", fetch_rows);
	ATF_REQUIRE_MSG(op_rows == 1,
	    "Expected exactly one IBS-OP row, found %zu", op_rows);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_hwpmc_fetch_caps_and_width);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_pmcinfo_rows_present);
	return (atf_no_error());
}

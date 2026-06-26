/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Sponsored by: Advanced Micro Devices, Inc.
 * Author: davi.chavesazevedo@amd.com
 */

#include <sys/param.h>

#include <atf-c.h>

#include "ibs_hwpmc_utils.h"

#define IBS_FETCH_RATE	65536ULL
#define IBS_OP_RATE	65536ULL

ATF_TC(ibs_hwpmc_alloc_fetch_basic);
ATF_TC_HEAD(ibs_hwpmc_alloc_fetch_basic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate and release a system-sampling IBS Fetch PMC via "
	    "libpmc");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_alloc_fetch_basic, tc)
{
	pmc_id_t pmcid;

	pmcid = PMC_ID_INVALID;
	ibs_test_skip_unless_hwpmc_ibs();

	ATF_REQUIRE_MSG(pmc_allocate("IBS-FETCH", PMC_MODE_SS, 0,
	    0, &pmcid, IBS_FETCH_RATE) == 0,
	    "pmc_allocate(IBS-FETCH) failed: %s", strerror(errno));

	amd_test_release_pmc(pmcid);
}

ATF_TC(ibs_hwpmc_alloc_op_basic);
ATF_TC_HEAD(ibs_hwpmc_alloc_op_basic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate and release a system-sampling IBS Op PMC via libpmc");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_alloc_op_basic, tc)
{
	pmc_id_t pmcid;

	pmcid = PMC_ID_INVALID;
	ibs_test_skip_unless_hwpmc_ibs();

	ATF_REQUIRE_MSG(pmc_allocate("IBS-OP", PMC_MODE_SS, 0,
	    0, &pmcid, IBS_OP_RATE) == 0,
	    "pmc_allocate(IBS-OP) failed: %s", strerror(errno));

	amd_test_release_pmc(pmcid);
}

ATF_TC(ibs_hwpmc_alloc_reject_counting_mode);
ATF_TC_HEAD(ibs_hwpmc_alloc_reject_counting_mode, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Reject IBS allocation in counting mode; IBS is sampling-only "
	    "in libpmc");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_alloc_reject_counting_mode, tc)
{
	pmc_id_t pmcid;
	int error;

	pmcid = PMC_ID_INVALID;
	ibs_test_skip_unless_hwpmc_ibs();

	errno = 0;
	error = pmc_allocate("IBS-FETCH", PMC_MODE_SC, 0, 0, &pmcid,
	    IBS_FETCH_RATE);
	if (error == 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("IBS Fetch allocation unexpectedly succeeded in "
		    "counting mode");
	}
	ATF_CHECK(errno == EINVAL);
}

ATF_TC(ibs_hwpmc_alloc_reject_bad_rate);
ATF_TC_HEAD(ibs_hwpmc_alloc_reject_bad_rate, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Reject IBS sample rates below the libpmc/kernel minimum");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_alloc_reject_bad_rate, tc)
{
	pmc_id_t pmcid;
	int error;

	pmcid = PMC_ID_INVALID;
	ibs_test_skip_unless_hwpmc_ibs();

	errno = 0;
	error = pmc_allocate("IBS-OP", PMC_MODE_SS, 0, 0, &pmcid, 1024);
	if (error == 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("IBS Op allocation unexpectedly accepted a too-small "
		    "sample rate");
	}
	ATF_CHECK(errno == EINVAL);
}

ATF_TC(ibs_hwpmc_alloc_reject_unknown_qualifier);
ATF_TC_HEAD(ibs_hwpmc_alloc_reject_unknown_qualifier, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Reject unknown IBS qualifiers during libpmc event parsing");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_alloc_reject_unknown_qualifier, tc)
{
	pmc_id_t pmcid;
	int error;

	pmcid = PMC_ID_INVALID;
	ibs_test_skip_unless_hwpmc_ibs();

	errno = 0;
	error = pmc_allocate("IBS-OP,definitely_not_a_real_kw", PMC_MODE_SS, 0,
	    0, &pmcid, IBS_OP_RATE);
	if (error == 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("IBS Op allocation unexpectedly accepted an invalid "
		    "qualifier");
	}
	ATF_CHECK(errno == EINVAL);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_hwpmc_alloc_fetch_basic);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_alloc_op_basic);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_alloc_reject_counting_mode);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_alloc_reject_bad_rate);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_alloc_reject_unknown_qualifier);
	return (atf_no_error());
}

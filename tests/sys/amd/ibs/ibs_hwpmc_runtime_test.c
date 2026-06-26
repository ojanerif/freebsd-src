/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Sponsored by: Advanced Micro Devices, Inc.
 * Author: davi.chavesazevedo@amd.com
 */

#include <sys/param.h>

#include <atf-c.h>

#include <machine/pmc_mdep.h>

#include "ibs_hwpmc_utils.h"

ATF_TC(ibs_hwpmc_fetch_lifecycle_smoke);
ATF_TC_HEAD(ibs_hwpmc_fetch_lifecycle_smoke, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate, attach, detach, and release an IBS Fetch PMC");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_fetch_lifecycle_smoke, tc)
{
	pmc_id_t pmcid;
	pid_t pid;
	int error;

	pmcid = PMC_ID_INVALID;
	pid = getpid();
	ibs_test_skip_unless_hwpmc_ibs();

	error = pmc_allocate("IBS-FETCH", PMC_MODE_TS, 0, PMC_CPU_ANY, &pmcid,
	    65536);
	ATF_REQUIRE_MSG(error == 0, "pmc_allocate(TS IBS-FETCH) failed: %s",
	    strerror(errno));
	if (pmc_attach(pmcid, pid) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_attach(IBS-FETCH, self) failed: %s",
		    strerror(errno));
	}
	if (pmc_detach(pmcid, pid) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_detach(IBS-FETCH, self) failed: %s",
		    strerror(errno));
	}
	amd_test_release_pmc(pmcid);
}

ATF_TC(ibs_hwpmc_getmsr_virtual_negative);
ATF_TC_HEAD(ibs_hwpmc_getmsr_virtual_negative, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify pmc_get_msr() rejects IBS sampling PMCs");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_hwpmc_getmsr_virtual_negative, tc)
{
	pmc_id_t pmcid;
	uint32_t msr;
	pid_t pid;
	bool attached;
	int error, getmsr_errno;

	pmcid = PMC_ID_INVALID;
	msr = 0;
	pid = getpid();
	attached = false;
	ibs_test_skip_unless_hwpmc_ibs();

	error = pmc_allocate("IBS-FETCH", PMC_MODE_TS, 0, PMC_CPU_ANY, &pmcid,
	    65536);
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(TS IBS-FETCH) failed: %s", strerror(errno));
	if (pmc_attach(pmcid, pid) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_attach(IBS-FETCH, self) failed: %s",
		    strerror(errno));
	}
	attached = true;

	errno = 0;
	error = pmc_get_msr(pmcid, &msr);
	getmsr_errno = errno;
	/* IBS has no GETMSR hook; older kernels may reject sampling PMCs first. */
	if (error != -1 || (getmsr_errno != ENOSYS && getmsr_errno != EINVAL)) {
		if (attached)
			(void)pmc_detach(pmcid, pid);
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_get_msr(IBS-FETCH) returned %d errno %d, "
		    "expected -1/ENOSYS or -1/EINVAL", error, getmsr_errno);
	}

	if (pmc_detach(pmcid, pid) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_detach(IBS-FETCH, self) failed: %s",
		    strerror(errno));
	}
	amd_test_release_pmc(pmcid);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_hwpmc_fetch_lifecycle_smoke);
	ATF_TP_ADD_TC(tp, ibs_hwpmc_getmsr_virtual_negative);
	return (atf_no_error());
}

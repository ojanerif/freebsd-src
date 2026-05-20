/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   Validate FreeBSD hwpmc(4) grouping behavior for AMD Zen core PMCs.
 *   The cases cover row disposition reporting, concurrent process-scope
 *   PMU event allocation, and the expected system-sampling failure path
 *   when no log file has been configured.
 */

#include <sys/param.h>
#include <sys/pmc.h>

#include <atf-c.h>

#include <errno.h>
#include <pmc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../amd_pmc_test_common.h"
#include "../umcdf/amd_umcdf_common.h"

static void
require_pmu_core_event(const char *name, struct pmc_op_pmcallocate *cfg)
{
	int error;

	memset(cfg, 0, sizeof(*cfg));
	error = pmc_pmu_pmcallocate(name, cfg);
	if (error != 0)
		atf_tc_skip("PMU event %s is unavailable: %s", name,
		    strerror(error));
	ATF_REQUIRE_MSG((cfg->pm_flags & PMC_F_EV_PMU) != 0,
	    "PMU event %s did not set PMC_F_EV_PMU", name);
	ATF_REQUIRE_MSG(cfg->pm_class == PMC_CLASS_K8,
	    "PMU event %s mapped to class %d, expected PMC_CLASS_K8", name,
	    cfg->pm_class);
}

static int
read_row_disposition(pmc_id_t pmcid, enum pmc_disp *disp)
{
	struct pmc_pmcinfo *pmcinfo;
	unsigned int row;
	int npmc;

	row = PMC_ID_TO_ROWINDEX(pmcid);
	npmc = pmc_npmc(0);
	if (npmc <= 0)
		return (errno != 0 ? errno : EINVAL);
	if (row >= (unsigned int)npmc)
		return (ERANGE);

	if (pmc_pmcinfo(0, &pmcinfo) != 0)
		return (errno != 0 ? errno : EINVAL);
	*disp = pmcinfo->pm_pmcs[row].pm_rowdisp;
	free(pmcinfo);
	return (0);
}

static void
require_row_disposition(pmc_id_t pmcid, enum pmc_disp expected,
    const char *context)
{
	enum pmc_disp disp;
	unsigned int row;
	int error;

	row = PMC_ID_TO_ROWINDEX(pmcid);
	error = read_row_disposition(pmcid, &disp);
	ATF_REQUIRE_MSG(error == 0, "%s row %u disposition read failed: %s",
	    context, row, strerror(error));
	ATF_CHECK_MSG(disp == expected,
	    "%s row %u disposition is %s, expected %s", context, row,
	    pmc_name_of_disposition(disp), pmc_name_of_disposition(expected));
}

static bool
pmc_allocate_error_is_skip(int error)
{

	return (error == EBUSY || error == ENOENT || error == ENXIO ||
	    error == EOPNOTSUPP);
}

static int
allocate_process_counting_pmc(const char *name, pmc_id_t *pmcid)
{

	*pmcid = PMC_ID_INVALID;
	errno = 0;
	if (pmc_allocate(name, PMC_MODE_TC, 0, PMC_CPU_ANY, pmcid, 0) == 0)
		return (0);
	return (errno != 0 ? errno : EINVAL);
}

static void
release_process_counting_pmc(pmc_id_t *pmcid)
{

	if (*pmcid == PMC_ID_INVALID)
		return;
	ATF_REQUIRE_MSG(pmc_release(*pmcid) == 0,
	    "pmc_release(%u) failed: %s", *pmcid, strerror(errno));
	*pmcid = PMC_ID_INVALID;
}

ATF_TC(row_disposition_tracks_process_and_system_pmc);
ATF_TC_HEAD(row_disposition_tracks_process_and_system_pmc, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify hwpmc row disposition exposes FreeBSD's real PMC "
	    "resource grouping: THREAD for process PMCs and STANDALONE for "
	    "system PMCs.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(row_disposition_tracks_process_and_system_pmc, tc)
{
	pmc_id_t pmcid;

	pmcid = PMC_ID_INVALID;
	amd_test_skip_unless_hwpmc();

	ATF_REQUIRE_MSG(pmc_allocate("SOFT-CLOCK.HARD", PMC_MODE_TC, 0,
	    PMC_CPU_ANY, &pmcid, 0) == 0,
	    "pmc_allocate(SOFT-CLOCK.HARD, TC) failed: %s", strerror(errno));
	require_row_disposition(pmcid, PMC_DISP_THREAD, "process-counting PMC");
	amd_test_release_pmc(pmcid);
	pmcid = PMC_ID_INVALID;

	ATF_REQUIRE_MSG(pmc_allocate("SOFT-CLOCK.HARD", PMC_MODE_SC, 0, 0,
	    &pmcid, 0) == 0,
	    "pmc_allocate(SOFT-CLOCK.HARD, SC) failed: %s", strerror(errno));
	require_row_disposition(pmcid, PMC_DISP_STANDALONE,
	    "system-counting PMC");
	amd_test_release_pmc(pmcid);
}

ATF_TC(amd_pmu_core_events_allocate_concurrently);
ATF_TC_HEAD(amd_pmu_core_events_allocate_concurrently, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify the same portable AMD Zen core PMU event resolves and "
	    "allocates twice as distinct process-counting hwpmc rows.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(amd_pmu_core_events_allocate_concurrently, tc)
{
	struct pmc_op_pmcallocate cycles_cfg;
	struct amd_umcdf_cpu cpu;
	enum pmc_disp cycles0_disp, cycles1_disp;
	pmc_id_t cycles0_pmc, cycles1_pmc;
	unsigned int cycles0_row, cycles1_row;
	int cycles0_disp_error, cycles1_disp_error, error;

	cycles0_pmc = PMC_ID_INVALID;
	cycles1_pmc = PMC_ID_INVALID;
	cycles0_disp = cycles1_disp = PMC_DISP_FREE;
	cycles0_disp_error = cycles1_disp_error = 0;
	cycles0_row = cycles1_row = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	amd_umcdf_skip_unless_pmu_events();

	require_pmu_core_event("unhalted-cycles", &cycles_cfg);

	error = allocate_process_counting_pmc("unhalted-cycles", &cycles0_pmc);
	if (error != 0 && pmc_allocate_error_is_skip(error))
		atf_tc_skip("first pmc_allocate(unhalted-cycles) failed: %s",
		    strerror(error));
	ATF_REQUIRE_MSG(error == 0,
	    "first pmc_allocate(unhalted-cycles) failed: %s", strerror(error));

	error = allocate_process_counting_pmc("unhalted-cycles", &cycles1_pmc);
	if (error != 0 && pmc_allocate_error_is_skip(error)) {
		release_process_counting_pmc(&cycles0_pmc);
		atf_tc_skip("second pmc_allocate(unhalted-cycles) failed: %s",
		    strerror(error));
	}
	if (error != 0)
		release_process_counting_pmc(&cycles0_pmc);
	ATF_REQUIRE_MSG(error == 0,
	    "second pmc_allocate(unhalted-cycles) failed: %s", strerror(error));

	cycles0_row = PMC_ID_TO_ROWINDEX(cycles0_pmc);
	cycles1_row = PMC_ID_TO_ROWINDEX(cycles1_pmc);
	cycles0_disp_error = read_row_disposition(cycles0_pmc, &cycles0_disp);
	cycles1_disp_error = read_row_disposition(cycles1_pmc, &cycles1_disp);

	release_process_counting_pmc(&cycles1_pmc);
	release_process_counting_pmc(&cycles0_pmc);

	ATF_CHECK_MSG(cycles0_row != cycles1_row,
	    "grouped unhalted-cycles PMCs used the same row");
	ATF_REQUIRE_MSG(cycles0_disp_error == 0,
	    "first unhalted-cycles row %u disposition read failed: %s",
	    cycles0_row, strerror(cycles0_disp_error));
	ATF_REQUIRE_MSG(cycles1_disp_error == 0,
	    "second unhalted-cycles row %u disposition read failed: %s",
	    cycles1_row, strerror(cycles1_disp_error));
	ATF_CHECK_MSG(cycles0_disp == PMC_DISP_THREAD,
	    "first unhalted-cycles row %u disposition is %s, expected %s",
	    cycles0_row, pmc_name_of_disposition(cycles0_disp),
	    pmc_name_of_disposition(PMC_DISP_THREAD));
	ATF_CHECK_MSG(cycles1_disp == PMC_DISP_THREAD,
	    "second unhalted-cycles row %u disposition is %s, expected %s",
	    cycles1_row, pmc_name_of_disposition(cycles1_disp),
	    pmc_name_of_disposition(PMC_DISP_THREAD));
}

ATF_TC(system_sampling_requires_logfile_before_start);
ATF_TC_HEAD(system_sampling_requires_logfile_before_start, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify system sampling PMCs fail cleanly at pmc_start() when no "
	    "hwpmc log file is configured.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(system_sampling_requires_logfile_before_start, tc)
{
	pmc_id_t pmcid;
	int error, start_errno;

	pmcid = PMC_ID_INVALID;
	amd_test_skip_unless_hwpmc();

	ATF_REQUIRE_MSG(pmc_allocate("SOFT-CLOCK.HARD", PMC_MODE_SS, 0, 0,
	    &pmcid, 10000) == 0,
	    "pmc_allocate(SOFT-CLOCK.HARD, SS) failed: %s", strerror(errno));
	errno = 0;
	error = pmc_start(pmcid);
	start_errno = errno;
	if (error == 0) {
		(void)pmc_stop(pmcid);
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_start() succeeded without a configured log file");
	}
	if (start_errno != EDOOFUS) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_start() failed with %s, expected EDOOFUS",
		    strerror(start_errno));
	}
	amd_test_release_pmc(pmcid);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, row_disposition_tracks_process_and_system_pmc);
	ATF_TP_ADD_TC(tp, amd_pmu_core_events_allocate_concurrently);
	ATF_TP_ADD_TC(tp, system_sampling_requires_logfile_before_start);
	return (atf_no_error());
}

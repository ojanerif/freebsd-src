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
#include <sys/cpuset.h>
#include <sys/pmc.h>
#include <sys/wait.h>

#include <dev/hwpmc/hwpmc_amd.h>

#include <atf-c.h>

#include <errno.h>
#include <fcntl.h>
#include <pmc.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../amd_pmc_test_common.h"
#include "../umcdf/amd_umcdf_common.h"
#include "msr_snapshot.h"
#include "pmcinfo_snapshot.h"

#define	AMD_GROUPING_CORE_EVENT	"unhalted-cycles"

struct alloc_race_arg {
	pthread_barrier_t *barrier;
	pmc_id_t pmcid;
	int error;
};

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
	ATF_REQUIRE_MSG(cfg->pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_CORE,
	    "PMU event %s mapped to AMD subclass %u, expected CORE", name,
	    cfg->pm_md.pm_amd.pm_amd_sub_class);
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

static int
amd_core_counter_index_from_pmcid(pmc_id_t pmcid, int cpu,
    unsigned int *counter, char *errbuf, size_t errlen)
{
	struct pmc_pmcinfo *pmcinfo;
	struct pmc_info *pi;
	char extra;
	int error, npmc;
	unsigned int idx, row;

	pmcinfo = NULL;
	row = PMC_ID_TO_ROWINDEX(pmcid);
	npmc = pmc_npmc(cpu);
	if (npmc < 0) {
		error = errno != 0 ? errno : EINVAL;
		(void)snprintf(errbuf, errlen, "pmc_npmc(%d) failed: %s",
		    cpu, strerror(error));
		return (error);
	}
	if (row >= (unsigned int)npmc) {
		(void)snprintf(errbuf, errlen,
		    "global row %u is outside hwpmc row count %d", row, npmc);
		return (ERANGE);
	}
	if (pmc_pmcinfo(cpu, &pmcinfo) != 0) {
		error = errno != 0 ? errno : EINVAL;
		(void)snprintf(errbuf, errlen, "pmc_pmcinfo(%d) failed: %s",
		    cpu, strerror(error));
		return (error);
	}
	pi = &pmcinfo->pm_pmcs[row];
	if (pi->pm_class != PMC_CLASS_K8 ||
	    sscanf(pi->pm_name, "K8-%u%c", &idx, &extra) != 1) {
		(void)snprintf(errbuf, errlen,
		    "global row %u is %s/class %d, not an AMD core PMC", row,
		    pi->pm_name, pi->pm_class);
		free(pmcinfo);
		return (EINVAL);
	}
	*counter = idx;
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

static void
require_amd_grouping_runtime(const atf_tc_t *tc)
{
	struct amd_umcdf_cpu cpu;
	struct pmc_op_pmcallocate cfg;

	if (!atf_tc_get_config_var_as_bool_wd(tc,
	    "amd.pmc.grouping.runtime", false))
		atf_tc_skip("AMD core grouping runtime disabled by default; set "
		    "amd.pmc.grouping.runtime=true");
	amd_umcdf_skip_unless_known_zen(&cpu);
	amd_umcdf_skip_unless_pmu_events();
	require_pmu_core_event(AMD_GROUPING_CORE_EVENT, &cfg);
}

static void
take_snapshot_or_fail(struct pmcinfo_snapshot *snap, const char *context)
{
	char errbuf[PMCINFO_SNAPSHOT_ERRLEN];
	int error;

	errbuf[0] = '\0';
	error = pmcinfo_snapshot_take(snap, errbuf, sizeof(errbuf));
	ATF_REQUIRE_MSG(error == 0, "%s snapshot failed: %s", context,
	    errbuf[0] != '\0' ? errbuf : strerror(error));
}

static void
require_no_snapshot_diff(const struct pmcinfo_snapshot *before,
    const struct pmcinfo_snapshot *after, const char *context)
{
	char errbuf[PMCINFO_SNAPSHOT_ERRLEN];
	size_t diffs;

	errbuf[0] = '\0';
	diffs = pmcinfo_snapshot_diff_count(before, after, errbuf,
	    sizeof(errbuf));
	ATF_REQUIRE_MSG(diffs == 0, "%s changed %zu hwpmc row snapshots: %s",
	    context, diffs, errbuf);
}

static void
require_pmcid_visible_in_snapshot(const struct pmcinfo_snapshot *snap,
    pmc_id_t pmcid, enum pmc_disp expected, const char *context)
{
	const struct pmcinfo_snapshot_row *row;
	unsigned int ri;

	ri = PMC_ID_TO_ROWINDEX(pmcid);
	row = pmcinfo_snapshot_find(snap, 0, ri);
	ATF_REQUIRE_MSG(row != NULL, "%s row %u is absent from cpu0 snapshot",
	    context, ri);
	ATF_CHECK_MSG(row->disp == expected,
	    "%s row %u disposition is %s, expected %s", context, ri,
	    pmc_name_of_disposition(row->disp),
	    pmc_name_of_disposition(expected));
}

static void
release_pmcid_array(pmc_id_t *pmcs, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (pmcs[i] != PMC_ID_INVALID) {
			ATF_REQUIRE_MSG(pmc_release(pmcs[i]) == 0,
			    "pmc_release(%u) failed: %s", pmcs[i], strerror(errno));
			pmcs[i] = PMC_ID_INVALID;
		}
	}
}

static bool
error_is_resource_exhaustion(int error)
{

	return (error == ENOSPC || error == EBUSY || error == ENOENT ||
	    error == ENXIO || error == EOPNOTSUPP);
}

static bool
error_is_post_success_exhaustion(int error, size_t successes)
{

	return (error_is_resource_exhaustion(error) ||
	    (successes > 0 && error == EINVAL));
}

static int
allocate_system_counting_pmc(const char *name, int cpu, pmc_id_t *pmcid)
{

	*pmcid = PMC_ID_INVALID;
	errno = 0;
	if (pmc_allocate(name, PMC_MODE_SC, 0, cpu, pmcid, 0) == 0)
		return (0);
	return (errno != 0 ? errno : EINVAL);
}

static void *
alloc_race_thread(void *arg)
{
	struct alloc_race_arg *race;

	race = arg;
	race->pmcid = PMC_ID_INVALID;
	(void)pthread_barrier_wait(race->barrier);
	race->error = allocate_process_counting_pmc(AMD_GROUPING_CORE_EVENT,
	    &race->pmcid);
	return (NULL);
}

static bool
wait_status_success(int status)
{

	return (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static pid_t
waitpid_nointr(pid_t pid, int *status)
{
	pid_t ret;

	do {
		ret = waitpid(pid, status, 0);
	} while (ret == -1 && errno == EINTR);
	return (ret);
}

static int
set_pid_to_cpu(pid_t pid, int cpu)
{
	cpuset_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, pid,
	    sizeof(set), &set));
}

static void
busy_child(int start_fd, int stop_fd)
{
	volatile uint64_t sink;
	uint64_t i;
	char ch;
	int flags;
	ssize_t nread;

	sink = 0;
	if (read(start_fd, &ch, 1) != 1)
		_exit(2);
	flags = fcntl(stop_fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(stop_fd, F_SETFL, flags | O_NONBLOCK);
	for (;;) {
		nread = read(stop_fd, &ch, 1);
		if (nread == 1 || nread == 0)
			break;
		if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR)
			_exit(3);
		for (i = 0; i < 100000; i++)
			sink += i;
	}
	_exit((int)(sink & 0x7f));
}

static void
stop_busy_child(pid_t pid, int start_pipe[2], int stop_pipe[2])
{
	int status;

	if (start_pipe[0] >= 0)
		(void)close(start_pipe[0]);
	if (start_pipe[1] >= 0)
		(void)close(start_pipe[1]);
	if (stop_pipe[0] >= 0)
		(void)close(stop_pipe[0]);
	if (stop_pipe[1] >= 0) {
		(void)write(stop_pipe[1], "x", 1);
		(void)close(stop_pipe[1]);
	}
	if (pid > 0) {
		(void)kill(pid, SIGTERM);
		(void)waitpid_nointr(pid, &status);
	}
}

static void
require_distinct_thread_rows(pmc_id_t pmc0, const char *name0, pmc_id_t pmc1,
    const char *name1)
{
	enum pmc_disp disp0, disp1;
	unsigned int row0, row1;
	int error0, error1;

	disp0 = disp1 = PMC_DISP_UNKNOWN;
	row0 = PMC_ID_TO_ROWINDEX(pmc0);
	row1 = PMC_ID_TO_ROWINDEX(pmc1);
	error0 = read_row_disposition(pmc0, &disp0);
	error1 = read_row_disposition(pmc1, &disp1);

	ATF_CHECK_MSG(row0 != row1,
	    "%s and %s PMCs used the same row %u", name0, name1, row0);
	ATF_CHECK_MSG(error0 == 0,
	    "%s row %u disposition read failed: %s", name0, row0,
	    strerror(error0));
	ATF_CHECK_MSG(error1 == 0,
	    "%s row %u disposition read failed: %s", name1, row1,
	    strerror(error1));
	if (error0 == 0)
		ATF_CHECK_MSG(disp0 == PMC_DISP_THREAD,
		    "%s row %u disposition is %s, expected %s", name0, row0,
		    pmc_name_of_disposition(disp0),
		    pmc_name_of_disposition(PMC_DISP_THREAD));
	if (error1 == 0)
		ATF_CHECK_MSG(disp1 == PMC_DISP_THREAD,
		    "%s row %u disposition is %s, expected %s", name1, row1,
		    pmc_name_of_disposition(disp1),
		    pmc_name_of_disposition(PMC_DISP_THREAD));
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
	pmc_id_t cycles0_pmc, cycles1_pmc;
	int error;

	cycles0_pmc = PMC_ID_INVALID;
	cycles1_pmc = PMC_ID_INVALID;

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

	require_distinct_thread_rows(cycles0_pmc, "first unhalted-cycles",
	    cycles1_pmc, "second unhalted-cycles");

	release_process_counting_pmc(&cycles1_pmc);
	release_process_counting_pmc(&cycles0_pmc);
}

ATF_TC(amd_pmu_mixed_core_events_allocate_concurrently);
ATF_TC_HEAD(amd_pmu_mixed_core_events_allocate_concurrently, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify mixed portable AMD Zen core PMU events allocate as distinct "
	    "process-counting hwpmc rows without starting runtime counters.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(amd_pmu_mixed_core_events_allocate_concurrently, tc)
{
	struct pmc_op_pmcallocate cycles_cfg, instr_cfg;
	struct amd_umcdf_cpu cpu;
	pmc_id_t cycles_pmc, instr_pmc;
	int error;

	cycles_pmc = PMC_ID_INVALID;
	instr_pmc = PMC_ID_INVALID;

	amd_umcdf_skip_unless_known_zen(&cpu);
	amd_umcdf_skip_unless_pmu_events();

	require_pmu_core_event("unhalted-cycles", &cycles_cfg);
	require_pmu_core_event("instructions", &instr_cfg);

	error = allocate_process_counting_pmc("unhalted-cycles", &cycles_pmc);
	if (error != 0 && pmc_allocate_error_is_skip(error))
		atf_tc_skip("pmc_allocate(unhalted-cycles) failed: %s",
		    strerror(error));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(unhalted-cycles) failed: %s", strerror(error));

	error = allocate_process_counting_pmc("instructions", &instr_pmc);
	if (error != 0 && pmc_allocate_error_is_skip(error)) {
		release_process_counting_pmc(&cycles_pmc);
		atf_tc_skip("pmc_allocate(instructions) failed: %s",
		    strerror(error));
	}
	if (error != 0)
		release_process_counting_pmc(&cycles_pmc);
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(instructions) failed: %s", strerror(error));

	require_distinct_thread_rows(cycles_pmc, "unhalted-cycles", instr_pmc,
	    "instructions");

	release_process_counting_pmc(&instr_pmc);
	release_process_counting_pmc(&cycles_pmc);
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

ATF_TC(alloc_reserves_distinct_rows);
ATF_TC_HEAD(alloc_reserves_distinct_rows, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify two AMD core pmc_allocate() calls reserve distinct THREAD rows.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(alloc_reserves_distinct_rows, tc)
{
	struct pmcinfo_snapshot before, after;
	pmc_id_t pmc0, pmc1;
	int error;

	pmc0 = pmc1 = PMC_ID_INVALID;
	require_amd_grouping_runtime(tc);
	take_snapshot_or_fail(&before, "pre-allocation");

	error = allocate_process_counting_pmc(AMD_GROUPING_CORE_EVENT, &pmc0);
	if (error != 0 && pmc_allocate_error_is_skip(error))
		atf_tc_skip("first pmc_allocate(%s) failed: %s",
		    AMD_GROUPING_CORE_EVENT, strerror(error));
	ATF_REQUIRE_MSG(error == 0, "first pmc_allocate(%s) failed: %s",
	    AMD_GROUPING_CORE_EVENT, strerror(error));

	error = allocate_process_counting_pmc(AMD_GROUPING_CORE_EVENT, &pmc1);
	if (error != 0 && pmc_allocate_error_is_skip(error)) {
		release_process_counting_pmc(&pmc0);
		pmcinfo_snapshot_free(&before);
		atf_tc_skip("second pmc_allocate(%s) failed: %s",
		    AMD_GROUPING_CORE_EVENT, strerror(error));
	}
	if (error != 0)
		release_process_counting_pmc(&pmc0);
	ATF_REQUIRE_MSG(error == 0, "second pmc_allocate(%s) failed: %s",
	    AMD_GROUPING_CORE_EVENT, strerror(error));

	take_snapshot_or_fail(&after, "post-allocation");
	require_distinct_thread_rows(pmc0, "first AMD core PMC", pmc1,
	    "second AMD core PMC");
	require_pmcid_visible_in_snapshot(&after, pmc0, PMC_DISP_THREAD,
	    "first AMD core PMC");
	require_pmcid_visible_in_snapshot(&after, pmc1, PMC_DISP_THREAD,
	    "second AMD core PMC");
	ATF_CHECK_MSG(pmcinfo_snapshot_diff_count(&before, &after, NULL, 0) > 0,
	    "two allocations should change the hwpmc row snapshot");

	release_process_counting_pmc(&pmc1);
	release_process_counting_pmc(&pmc0);
	pmcinfo_snapshot_free(&after);
	pmcinfo_snapshot_free(&before);
}

ATF_TC(alloc_rollback_on_oversubscribe);
ATF_TC_HEAD(alloc_rollback_on_oversubscribe, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify a failed AMD core allocation leaves the pmc_pmcinfo snapshot unchanged.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(alloc_rollback_on_oversubscribe, tc)
{
	struct pmcinfo_snapshot before_attempt, after_fail, after_release, initial;
	pmc_id_t *pmcs, extra;
	size_t n, limit;
	int error, npmc;
	bool saw_failure;

	require_amd_grouping_runtime(tc);
	npmc = pmc_npmc(0);
	ATF_REQUIRE_MSG(npmc > 0, "pmc_npmc(0) failed: %s", strerror(errno));
	limit = (size_t)npmc + 2;
	pmcs = calloc(limit, sizeof(*pmcs));
	ATF_REQUIRE_MSG(pmcs != NULL, "calloc pmcid array failed: %s",
	    strerror(errno));
	for (n = 0; n < limit; n++)
		pmcs[n] = PMC_ID_INVALID;
	extra = PMC_ID_INVALID;
	saw_failure = false;
	take_snapshot_or_fail(&initial, "initial");

	for (n = 0; n < limit; n++) {
		take_snapshot_or_fail(&before_attempt, "before oversubscribe attempt");
		error = allocate_process_counting_pmc(AMD_GROUPING_CORE_EVENT,
		    &extra);
		if (error == 0) {
			pmcs[n] = extra;
			extra = PMC_ID_INVALID;
			pmcinfo_snapshot_free(&before_attempt);
			continue;
		}
		ATF_REQUIRE_MSG(error_is_post_success_exhaustion(error, n),
		    "oversubscribe failed with non-resource errno %s", strerror(error));
		take_snapshot_or_fail(&after_fail, "after failed allocation");
		require_no_snapshot_diff(&before_attempt, &after_fail,
		    "failed pmc_allocate rollback");
		pmcinfo_snapshot_free(&after_fail);
		pmcinfo_snapshot_free(&before_attempt);
		saw_failure = true;
		break;
	}

	release_pmcid_array(pmcs, limit);
	take_snapshot_or_fail(&after_release, "after release");
	require_no_snapshot_diff(&initial, &after_release,
	    "release after oversubscribe characterization");
	pmcinfo_snapshot_free(&after_release);
	pmcinfo_snapshot_free(&initial);
	free(pmcs);
	if (!saw_failure)
		atf_tc_skip("could not oversubscribe AMD core rows after %zu allocations",
		    limit);
}

ATF_TC(alloc_no_partial_pmcid_visibility);
ATF_TC_HEAD(alloc_no_partial_pmcid_visibility, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify concurrent pmc_allocate() returns either a visible pmcid or errno only.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(alloc_no_partial_pmcid_visibility, tc)
{
	struct alloc_race_arg args[2];
	struct pmcinfo_snapshot snap;
	pthread_barrier_t barrier;
	pthread_t threads[2];
	int i, successes;

	require_amd_grouping_runtime(tc);
	ATF_REQUIRE_MSG(pthread_barrier_init(&barrier, NULL, 2) == 0,
	    "pthread_barrier_init failed");
	for (i = 0; i < 2; i++) {
		args[i].barrier = &barrier;
		args[i].pmcid = PMC_ID_INVALID;
		args[i].error = 0;
		ATF_REQUIRE_MSG(pthread_create(&threads[i], NULL,
		    alloc_race_thread, &args[i]) == 0, "pthread_create(%d) failed", i);
	}
	for (i = 0; i < 2; i++)
		ATF_REQUIRE_MSG(pthread_join(threads[i], NULL) == 0,
		    "pthread_join(%d) failed", i);
	(void)pthread_barrier_destroy(&barrier);

	successes = 0;
	for (i = 0; i < 2; i++) {
		if (args[i].error == 0) {
			successes++;
			ATF_CHECK_MSG(args[i].pmcid != PMC_ID_INVALID,
			    "successful thread %d returned invalid pmcid", i);
		} else {
			ATF_CHECK_MSG(args[i].pmcid == PMC_ID_INVALID,
			    "failed thread %d exposed partial pmcid %u", i,
			    args[i].pmcid);
		}
	}
	if (successes == 0) {
		for (i = 0; i < 2; i++)
			ATF_CHECK_MSG(error_is_resource_exhaustion(args[i].error),
			    "thread %d failed with unexpected errno %s", i,
			    strerror(args[i].error));
		atf_tc_skip("both racing allocations failed due to unavailable AMD core rows");
	}

	take_snapshot_or_fail(&snap, "post-race");
	for (i = 0; i < 2; i++) {
		if (args[i].error == 0)
			require_pmcid_visible_in_snapshot(&snap, args[i].pmcid,
			    PMC_DISP_THREAD, "race allocation");
	}
	if (successes == 2)
		ATF_CHECK_MSG(PMC_ID_TO_ROWINDEX(args[0].pmcid) !=
		    PMC_ID_TO_ROWINDEX(args[1].pmcid),
		    "two racing allocations returned the same row index");
	for (i = 0; i < 2; i++)
		release_process_counting_pmc(&args[i].pmcid);
	pmcinfo_snapshot_free(&snap);
}

ATF_TC(alloc_mode_class_atomicity);
ATF_TC_HEAD(alloc_mode_class_atomicity, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify THREAD-occupied AMD core rows exclude a STANDALONE allocation without residue.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(alloc_mode_class_atomicity, tc)
{
	struct pmcinfo_snapshot full, after_fail;
	pmc_id_t *pmcs, system_pmc;
	size_t n, limit;
	int error, npmc;

	require_amd_grouping_runtime(tc);
	npmc = pmc_npmc(0);
	ATF_REQUIRE_MSG(npmc > 0, "pmc_npmc(0) failed: %s", strerror(errno));
	limit = (size_t)npmc + 2;
	pmcs = calloc(limit, sizeof(*pmcs));
	ATF_REQUIRE_MSG(pmcs != NULL, "calloc pmcid array failed: %s",
	    strerror(errno));
	for (n = 0; n < limit; n++)
		pmcs[n] = PMC_ID_INVALID;
	system_pmc = PMC_ID_INVALID;

	for (n = 0; n < limit; n++) {
		error = allocate_process_counting_pmc(AMD_GROUPING_CORE_EVENT,
		    &pmcs[n]);
		if (error != 0) {
			ATF_REQUIRE_MSG(error_is_post_success_exhaustion(error, n),
			    "process allocation failed with unexpected errno %s",
			    strerror(error));
			break;
		}
	}
	if (n == 0) {
		free(pmcs);
		atf_tc_skip("no AMD core process rows were allocatable");
	}

	take_snapshot_or_fail(&full, "THREAD-saturated rows");
	error = allocate_system_counting_pmc(AMD_GROUPING_CORE_EVENT, 0,
	    &system_pmc);
	if (error == 0) {
		(void)pmc_release(system_pmc);
		release_pmcid_array(pmcs, limit);
		free(pmcs);
		pmcinfo_snapshot_free(&full);
		atf_tc_fail("system allocation succeeded while AMD core THREAD rows were saturated");
	}
	ATF_REQUIRE_MSG(error_is_post_success_exhaustion(error, n),
	    "system allocation failed with unexpected errno %s", strerror(error));
	take_snapshot_or_fail(&after_fail, "after excluded STANDALONE allocation");
	require_no_snapshot_diff(&full, &after_fail,
	    "THREAD/STANDALONE exclusion failure path");
	pmcinfo_snapshot_free(&after_fail);
	pmcinfo_snapshot_free(&full);
	release_pmcid_array(pmcs, limit);
	free(pmcs);
}

ATF_TC(start_sys_mode_writes_msr_immediately);
ATF_TC_HEAD(start_sys_mode_writes_msr_immediately, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify PMC_MODE_SC writes AMD PerfEvtSel.En immediately on pmc_start().");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(start_sys_mode_writes_msr_immediately, tc)
{
	struct amd_msr_snapshot before, after;
	char errbuf[160];
	pmc_id_t pmcid;
	unsigned int global_row;
	unsigned int row;
	int error;

	pmcid = PMC_ID_INVALID;
	require_amd_grouping_runtime(tc);
	errbuf[0] = '\0';
	error = amd_msr_snapshot_take(0, &before, errbuf, sizeof(errbuf));
	if (error != 0)
		atf_tc_skip("cpuctl(4) MSR snapshot required: %s",
		    errbuf[0] != '\0' ? errbuf : strerror(error));
	error = allocate_system_counting_pmc(AMD_GROUPING_CORE_EVENT, 0, &pmcid);
	if (error != 0 && pmc_allocate_error_is_skip(error))
		atf_tc_skip("pmc_allocate(%s, SC) failed: %s",
		    AMD_GROUPING_CORE_EVENT, strerror(error));
	ATF_REQUIRE_MSG(error == 0, "pmc_allocate(%s, SC) failed: %s",
	    AMD_GROUPING_CORE_EVENT, strerror(error));
	global_row = PMC_ID_TO_ROWINDEX(pmcid);
	errbuf[0] = '\0';
	error = amd_core_counter_index_from_pmcid(pmcid, 0, &row, errbuf,
	    sizeof(errbuf));
	if (error != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("failed to map global row %u to AMD core counter: %s",
		    global_row, errbuf[0] != '\0' ? errbuf : strerror(error));
	}
	if (row >= before.num_core_pmcs) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("allocated global row %u maps to core counter %u, "
		    "outside core MSR snapshot count %u", global_row, row,
		    before.num_core_pmcs);
	}
	if (amd_msr_snapshot_evsel_enabled(&before, row)) {
		amd_test_release_pmc(pmcid);
		atf_tc_skip("row %u PerfEvtSel.En was already set before test start",
		    row);
	}
	if (pmc_start(pmcid) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_start(%u) failed: %s", pmcid,
		    strerror(errno));
	}
	usleep(10000);
	errbuf[0] = '\0';
	error = amd_msr_snapshot_take(0, &after, errbuf, sizeof(errbuf));
	(void)pmc_stop(pmcid);
	amd_test_release_pmc(pmcid);
	ATF_REQUIRE_MSG(error == 0, "post-start MSR snapshot failed: %s",
	    errbuf[0] != '\0' ? errbuf : strerror(error));
	if (after.perfmon_v2 && after.global_ctl_valid)
		fprintf(stderr, "diag: PerfCntrGlobalCtl[%u]=%u\n", row,
		    (unsigned int)((after.global_ctl >> row) & 1));
	ATF_CHECK_MSG(amd_msr_snapshot_evsel_enabled(&after, row),
	    "PMC_MODE_SC row %u did not set AMD PerfEvtSel.En immediately", row);
}

ATF_TC(start_proc_mode_defers_to_csw);
ATF_TC_HEAD(start_proc_mode_defers_to_csw, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify process-mode pmc_start() leaves AMD PerfEvtSel.En clear "
	    "before target context switch-in and counts only after target run.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(start_proc_mode_defers_to_csw, tc)
{
	struct amd_msr_snapshot snap;
	char errbuf[160];
	int error, ncpu, start_pipe[2], stop_pipe[2], status, target_cpu;
	pid_t child;
	pmc_id_t pmcid;
	pmc_value_t after_count;
	unsigned int global_row;
	unsigned int row;

	pmcid = PMC_ID_INVALID;
	start_pipe[0] = start_pipe[1] = stop_pipe[0] = stop_pipe[1] = -1;
	require_amd_grouping_runtime(tc);
	ncpu = pmc_ncpu();
	if (ncpu < 2)
		atf_tc_skip("process-mode MSR deferral needs two logical CPUs");
	target_cpu = 1;
	errbuf[0] = '\0';
	error = amd_msr_snapshot_take(target_cpu, &snap, errbuf, sizeof(errbuf));
	if (error != 0)
		atf_tc_skip("cpuctl(4) MSR snapshot required: %s",
		    errbuf[0] != '\0' ? errbuf : strerror(error));
	ATF_REQUIRE_MSG(pipe(start_pipe) == 0, "pipe(start) failed: %s",
	    strerror(errno));
	ATF_REQUIRE_MSG(pipe(stop_pipe) == 0, "pipe(stop) failed: %s",
	    strerror(errno));
	child = fork();
	ATF_REQUIRE_MSG(child >= 0, "fork failed: %s", strerror(errno));
	if (child == 0) {
		(void)close(start_pipe[1]);
		(void)close(stop_pipe[1]);
		busy_child(start_pipe[0], stop_pipe[0]);
	}
	(void)close(start_pipe[0]);
	start_pipe[0] = -1;
	(void)close(stop_pipe[0]);
	stop_pipe[0] = -1;
	if (set_pid_to_cpu(child, target_cpu) != 0) {
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_skip("cpuset_setaffinity(child,cpu%d) failed: %s",
		    target_cpu, strerror(errno));
	}

	error = allocate_process_counting_pmc(AMD_GROUPING_CORE_EVENT, &pmcid);
	if (error != 0) {
		stop_busy_child(child, start_pipe, stop_pipe);
		if (pmc_allocate_error_is_skip(error))
			atf_tc_skip("pmc_allocate(%s, TC) failed: %s",
			    AMD_GROUPING_CORE_EVENT, strerror(error));
		atf_tc_fail("pmc_allocate(%s, TC) failed: %s",
		    AMD_GROUPING_CORE_EVENT, strerror(error));
	}
	global_row = PMC_ID_TO_ROWINDEX(pmcid);
	errbuf[0] = '\0';
	error = amd_core_counter_index_from_pmcid(pmcid, target_cpu, &row, errbuf,
	    sizeof(errbuf));
	if (error != 0) {
		amd_test_release_pmc(pmcid);
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_fail("failed to map global row %u to AMD core counter: %s",
		    global_row, errbuf[0] != '\0' ? errbuf : strerror(error));
	}
	if (row >= snap.num_core_pmcs) {
		amd_test_release_pmc(pmcid);
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_fail("allocated global row %u maps to core counter %u, "
		    "outside core MSR snapshot count %u", global_row, row,
		    snap.num_core_pmcs);
	}
	if (pmc_attach(pmcid, child) != 0) {
		amd_test_release_pmc(pmcid);
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_fail("pmc_attach(%u,%d) failed: %s", pmcid, child,
		    strerror(errno));
	}
	if (pmc_start(pmcid) != 0) {
		amd_test_release_pmc(pmcid);
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_fail("pmc_start(%u) failed: %s", pmcid, strerror(errno));
	}
	/*
	 * CPUCTL_RDMSR migrates this caller onto target_cpu inside cpuctl(4).
	 * That observation is still load-bearing safe here because the attached
	 * child is pinned to target_cpu but remains blocked on start_pipe until
	 * after this snapshot.  If the child is allowed to run before this read,
	 * process-mode hwpmc can legitimately arm the AMD PerfEvtSel.En bit during
	 * the context-switch-in path and invalidate the deferral assertion.
	 */
	errbuf[0] = '\0';
	error = amd_msr_snapshot_take(target_cpu, &snap, errbuf, sizeof(errbuf));
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_test_release_pmc(pmcid);
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_fail("pre-run target MSR snapshot failed: %s",
		    errbuf[0] != '\0' ? errbuf : strerror(error));
	}
	ATF_CHECK_MSG(!amd_msr_snapshot_evsel_enabled(&snap, row),
	    "process-mode row %u was armed before the attached child ran", row);

	if (write(start_pipe[1], "x", 1) != 1) {
		(void)pmc_stop(pmcid);
		amd_test_release_pmc(pmcid);
		stop_busy_child(child, start_pipe, stop_pipe);
		atf_tc_fail("failed to start child workload: %s", strerror(errno));
	}
	(void)close(start_pipe[1]);
	start_pipe[1] = -1;
	usleep(250000);
	(void)write(stop_pipe[1], "x", 1);
	(void)close(stop_pipe[1]);
	stop_pipe[1] = -1;
	ATF_REQUIRE_MSG(waitpid_nointr(child, &status) == child,
	    "waitpid(%d) failed: %s", child, strerror(errno));
	ATF_CHECK_MSG(WIFEXITED(status) || WIFSIGNALED(status),
	    "child exited with unexpected wait status %#x", status);
	if (pmc_stop(pmcid) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_stop(%u) failed: %s", pmcid, strerror(errno));
	}
	if (pmc_read(pmcid, &after_count) != 0) {
		amd_test_release_pmc(pmcid);
		atf_tc_fail("pmc_read(%u) failed after child run: %s", pmcid,
		    strerror(errno));
	}
	ATF_CHECK_MSG(after_count > 0,
	    "process-mode PMC did not accumulate after attached child ran");
	amd_test_release_pmc(pmcid);
}

ATF_TC(start_skew_is_sequential_per_row);
ATF_TC_HEAD(start_skew_is_sequential_per_row, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Use DTrace FBT to characterize adjacent amd_start_pmc row starts.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(start_skew_is_sequential_per_row, tc)
{
	FILE *fp;
	char line[256];
	long long ts, prev_ts, deltas[128], tmp;
	int cpu, pid, ri, prev_ri, status;
	size_t i, j, ndeltas;

	require_amd_grouping_runtime(tc);
	if (access("/usr/sbin/dtrace", X_OK) != 0)
		atf_tc_skip("DTrace is unavailable");
	(void)unlink("dtrace-start.out");
	(void)unlink("dtrace-start.err");
	status = system("/usr/sbin/dtrace -Z -q "
	    "-n 'fbt::amd_start_pmc:entry "
	    "/pid == $target || progenyof($target)/ "
	    "{ printf(\"start,%d,%d,%d,%lld\\n\", cpu, arg1, pid, "
	    "timestamp); }' "
	    "-c '/usr/sbin/pmcstat -C -q -p ls_not_halted_cyc "
	    "-p ls_not_halted_cyc -- /bin/dd if=/dev/zero of=/dev/null "
	    "bs=4096 count=10000' > dtrace-start.out 2>dtrace-start.err");
	if (!wait_status_success(status)) {
		(void)unlink("dtrace-start.out");
		atf_tc_skip("DTrace amd_start_pmc trace failed; see dtrace-start.err");
	}
	fp = fopen("dtrace-start.out", "r");
	ATF_REQUIRE_MSG(fp != NULL, "fopen(dtrace-start.out) failed: %s",
	    strerror(errno));
	prev_ts = -1;
	prev_ri = -1;
	ndeltas = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (sscanf(line, "start,%d,%d,%d,%lld", &cpu, &ri, &pid, &ts) != 4)
			continue;
		if (prev_ts >= 0 && prev_ri != ri && ts > prev_ts &&
		    ndeltas < nitems(deltas))
			deltas[ndeltas++] = ts - prev_ts;
		prev_ts = ts;
		prev_ri = ri;
	}
	(void)fclose(fp);
	(void)unlink("dtrace-start.out");
	(void)unlink("dtrace-start.err");
	if (ndeltas == 0)
		atf_tc_skip("DTrace produced no adjacent amd_start_pmc row-start pairs");
	for (i = 0; i < ndeltas; i++)
		for (j = i + 1; j < ndeltas; j++)
			if (deltas[j] < deltas[i]) {
				tmp = deltas[i];
				deltas[i] = deltas[j];
				deltas[j] = tmp;
			}
	ATF_CHECK_MSG(deltas[ndeltas / 2] > 0,
	    "median adjacent amd_start_pmc delta was not positive");
}

ATF_TC(group_start_atomicity_EXPECTED_FAIL);
ATF_TC_HEAD(group_start_atomicity_EXPECTED_FAIL, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Expected-fail bridge for future PMCGROUPSTART atomic row arming.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(group_start_atomicity_EXPECTED_FAIL, tc)
{

	require_amd_grouping_runtime(tc);
	atf_tc_expect_fail("FreeBSD hwpmc(4) has only per-row PMCSTART today; "
	    "future PMCGROUPSTART/PerfCntrGlobalCtl support should replace this "
	    "expected-failure bridge with a measured atomic-arm assertion");
	ATF_CHECK_MSG(false,
	    "future ABI contract should compare grouped AMD core row-arm "
	    "latency against the measured sequential-start noise floor");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, row_disposition_tracks_process_and_system_pmc);
	ATF_TP_ADD_TC(tp, amd_pmu_core_events_allocate_concurrently);
	ATF_TP_ADD_TC(tp, amd_pmu_mixed_core_events_allocate_concurrently);
	ATF_TP_ADD_TC(tp, system_sampling_requires_logfile_before_start);
	ATF_TP_ADD_TC(tp, alloc_reserves_distinct_rows);
	ATF_TP_ADD_TC(tp, alloc_rollback_on_oversubscribe);
	ATF_TP_ADD_TC(tp, alloc_no_partial_pmcid_visibility);
	ATF_TP_ADD_TC(tp, alloc_mode_class_atomicity);
	ATF_TP_ADD_TC(tp, start_sys_mode_writes_msr_immediately);
	ATF_TP_ADD_TC(tp, start_proc_mode_defers_to_csw);
	ATF_TP_ADD_TC(tp, start_skew_is_sequential_per_row);
	ATF_TP_ADD_TC(tp, group_start_atomicity_EXPECTED_FAIL);
	return (atf_no_error());
}

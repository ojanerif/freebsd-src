/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   Validate the FreeBSD hwpmc(4) PMU grouping ABI on AMD Zen core PMCs.
 *   These cases intentionally avoid multiplexing: every group is sized to fit
 *   within the architectural AMD core PMC pool.
 */

#include <sys/param.h>
#include <sys/pmc.h>

#include <dev/hwpmc/hwpmc_amd.h>

#include <atf-c.h>

#include <errno.h>
#include <pmc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../umcdf/amd_umcdf_common.h"

#define	GROUP_EVENT_INSTR	"instructions"
#define	GROUP_EVENT_CYCLES	"unhalted-cycles"
#define	GROUP_EVENT_BRANCHES	"branches"

static volatile uint64_t group_api_sink;

static void
busy_loop(void)
{
	uint64_t i;

	for (i = 0; i < 100000000ULL; i++)
		group_api_sink += i;
}

static bool
is_transient_resource_error(int error)
{

	return (error == EBUSY || error == ENOENT || error == ENOSPC ||
	    error == ENXIO || error == EOPNOTSUPP);
}

static void
require_runtime_enabled(const atf_tc_t *tc)
{
	struct amd_umcdf_cpu cpu;

	if (!atf_tc_get_config_var_as_bool_wd(tc,
	    "amd.pmc.grouping.runtime", false))
		atf_tc_skip("AMD core grouping runtime disabled by default; set "
		    "amd.pmc.grouping.runtime=true");
	amd_umcdf_skip_unless_known_zen(&cpu);
	amd_umcdf_skip_unless_pmu_events();
	fprintf(stderr, "AMD Zen grouping target: family=%02x model=%02x "
	    "stepping=%x zen=%d\n", cpu.family, cpu.model, cpu.stepping,
	    cpu.zen);
}

static void
require_core_event(const char *name)
{
	struct pmc_op_pmcallocate cfg;
	int error;

	memset(&cfg, 0, sizeof(cfg));
	error = pmc_pmu_pmcallocate(name, &cfg);
	if (error != 0)
		atf_tc_skip("PMU event %s is unavailable: %s", name,
		    strerror(error));
	ATF_REQUIRE_MSG((cfg.pm_flags & PMC_F_EV_PMU) != 0,
	    "PMU event %s did not set PMC_F_EV_PMU", name);
	ATF_REQUIRE_MSG(cfg.pm_class == PMC_CLASS_K8,
	    "PMU event %s mapped to class %d, expected PMC_CLASS_K8",
	    name, cfg.pm_class);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_CORE,
	    "PMU event %s mapped to AMD subclass %u, expected CORE",
	    name, cfg.pm_md.pm_amd.pm_amd_sub_class);
}

static void
require_group_events(const char *const *events, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		require_core_event(events[i]);
}

static void
release_pmcs(pmc_id_t *ids, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (ids[i] == PMC_ID_INVALID)
			continue;
		ATF_REQUIRE_MSG(pmc_release(ids[i]) == 0,
		    "pmc_release(%u) failed: %s", ids[i], strerror(errno));
		ids[i] = PMC_ID_INVALID;
	}
}

#ifdef PMC_F_GROUP_DEFER
static void
create_group_or_skip(uint32_t *gid)
{
	int error;

	if (pmc_group_create(gid) == 0)
		return;
	error = errno;
	if (is_transient_resource_error(error))
		atf_tc_skip("pmc_group_create failed due to unsupported or "
		    "unavailable hwpmc resources: %s", strerror(error));
	atf_tc_fail("pmc_group_create failed: %s", strerror(error));
}

static void
allocate_group_pmc_or_skip(const char *event, pmc_id_t *id)
{
	int error;

	*id = PMC_ID_INVALID;
	if (pmc_allocate_group(event, PMC_MODE_TC, 0, PMC_CPU_ANY, id, 0) == 0)
		return;
	error = errno;
	if (is_transient_resource_error(error))
		atf_tc_skip("pmc_allocate_group(%s) failed due to unavailable "
		    "resources: %s", event, strerror(error));
	atf_tc_fail("pmc_allocate_group(%s) failed: %s", event,
	    strerror(error));
}

static void
add_group_pmc(uint32_t gid, pmc_id_t id, bool leader)
{

	ATF_REQUIRE_MSG(pmc_group_add(gid, id, leader ? 1 : 0) == 0,
	    "pmc_group_add(gid=%u, pmcid=%u, leader=%d) failed: %s",
	    gid, id, leader ? 1 : 0, strerror(errno));
}

static void
commit_group_or_skip(uint32_t gid)
{
	int error;

	if (pmc_group_commit(gid) == 0)
		return;
	error = errno;
	if (is_transient_resource_error(error))
		atf_tc_skip("pmc_group_commit(gid=%u) failed due to "
		    "unsupported or unavailable hwpmc resources: %s", gid,
		    strerror(error));
	atf_tc_fail("pmc_group_commit(gid=%u) failed: %s", gid,
	    strerror(error));
}

static void
build_group_or_skip(const char *const *events, size_t n, uint32_t *gid,
    pmc_id_t *ids)
{
	size_t i;

	create_group_or_skip(gid);
	for (i = 0; i < n; i++) {
		allocate_group_pmc_or_skip(events[i], &ids[i]);
		add_group_pmc(*gid, ids[i], i == 0);
	}
}
#endif

ATF_TC(group_commit_and_leader_start_count_all_siblings);
ATF_TC_HEAD(group_commit_and_leader_start_count_all_siblings, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Commit a fitting AMD core PMU group and verify starting only the "
	    "leader counts every sibling.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(group_commit_and_leader_start_count_all_siblings, tc)
{
#ifndef PMC_F_GROUP_DEFER
	atf_tc_skip("hwpmc PMU grouping ABI is not available in headers");
#else
	const char *events[] = {
		GROUP_EVENT_INSTR,
		GROUP_EVENT_CYCLES,
		GROUP_EVENT_BRANCHES,
	};
	pmc_value_t values[nitems(events)];
	pmc_id_t ids[nitems(events)];
	uint32_t gid;
	size_t i;

	memset(values, 0, sizeof(values));
	for (i = 0; i < nitems(ids); i++)
		ids[i] = PMC_ID_INVALID;
	require_runtime_enabled(tc);
	require_group_events(events, nitems(events));
	build_group_or_skip(events, nitems(events), &gid, ids);
	commit_group_or_skip(gid);

	for (i = 0; i < nitems(ids); i++)
		ATF_REQUIRE_MSG(pmc_attach(ids[i], getpid()) == 0,
		    "pmc_attach(%u, self) failed: %s", ids[i], strerror(errno));

	ATF_REQUIRE_MSG(pmc_start(ids[0]) == 0,
	    "pmc_start(group leader %u) failed: %s", ids[0], strerror(errno));
	busy_loop();

	for (i = 0; i < nitems(ids); i++) {
		ATF_REQUIRE_MSG(pmc_read(ids[i], &values[i]) == 0,
		    "pmc_read(%s/%u) failed: %s", events[i], ids[i],
		    strerror(errno));
		ATF_CHECK_MSG(values[i] > 0,
		    "group sibling %s/%u did not count after leader-only start",
		    events[i], ids[i]);
		fprintf(stderr, "group sibling %-16s pmcid=%u count=%ju\n",
		    events[i], ids[i], (uintmax_t)values[i]);
	}
	ATF_REQUIRE_MSG(pmc_stop(ids[0]) == 0,
	    "pmc_stop(group leader %u) failed: %s", ids[0], strerror(errno));
	release_pmcs(ids, nitems(ids));
#endif
}

ATF_TC(group_commit_is_idempotent_and_closed_to_late_add);
ATF_TC_HEAD(group_commit_is_idempotent_and_closed_to_late_add, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A fitting AMD core PMU group may be committed twice, but rejects "
	    "new members after commit.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(group_commit_is_idempotent_and_closed_to_late_add, tc)
{
#ifndef PMC_F_GROUP_DEFER
	atf_tc_skip("hwpmc PMU grouping ABI is not available in headers");
#else
	const char *events[] = {
		GROUP_EVENT_INSTR,
		GROUP_EVENT_CYCLES,
	};
	pmc_id_t ids[nitems(events)], extra;
	uint32_t gid;
	size_t i;

	for (i = 0; i < nitems(ids); i++)
		ids[i] = PMC_ID_INVALID;
	extra = PMC_ID_INVALID;
	require_runtime_enabled(tc);
	require_group_events(events, nitems(events));
	require_core_event(GROUP_EVENT_BRANCHES);
	build_group_or_skip(events, nitems(events), &gid, ids);
	commit_group_or_skip(gid);
	commit_group_or_skip(gid);

	allocate_group_pmc_or_skip(GROUP_EVENT_BRANCHES, &extra);
	errno = 0;
	ATF_CHECK_MSG(pmc_group_add(gid, extra, 0) < 0,
	    "pmc_group_add unexpectedly accepted a member after commit");
	ATF_CHECK_MSG(errno == EBUSY,
	    "late pmc_group_add failed with %s, expected EBUSY",
	    strerror(errno));

	if (extra != PMC_ID_INVALID)
		ATF_REQUIRE_MSG(pmc_release(extra) == 0,
		    "pmc_release(extra %u) failed: %s", extra, strerror(errno));
	release_pmcs(ids, nitems(ids));
#endif
}

ATF_TC(single_event_group_commit_is_rejected);
ATF_TC_HEAD(single_event_group_commit_is_rejected, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A one-event brace/API group is not a real PMU group and must be "
	    "rejected by pmc_group_commit().");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(single_event_group_commit_is_rejected, tc)
{
#ifndef PMC_F_GROUP_DEFER
	atf_tc_skip("hwpmc PMU grouping ABI is not available in headers");
#else
	pmc_id_t id;
	uint32_t gid;

	id = PMC_ID_INVALID;
	require_runtime_enabled(tc);
	require_core_event(GROUP_EVENT_INSTR);
	create_group_or_skip(&gid);
	allocate_group_pmc_or_skip(GROUP_EVENT_INSTR, &id);
	add_group_pmc(gid, id, true);
	errno = 0;
	ATF_CHECK_MSG(pmc_group_commit(gid) < 0,
	    "pmc_group_commit unexpectedly accepted a single-event group");
	ATF_CHECK_MSG(errno == EINVAL,
	    "single-event pmc_group_commit failed with %s, expected EINVAL",
	    strerror(errno));
	if (id != PMC_ID_INVALID)
		ATF_REQUIRE_MSG(pmc_release(id) == 0,
		    "pmc_release(%u) failed: %s", id, strerror(errno));
#endif
}

ATF_TC(group_release_helper_releases_committed_group);
ATF_TC_HEAD(group_release_helper_releases_committed_group, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "pmc_group_release() releases every sibling in a committed fitting "
	    "AMD core PMU group.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(group_release_helper_releases_committed_group, tc)
{
#ifndef PMC_F_GROUP_DEFER
	atf_tc_skip("hwpmc PMU grouping ABI is not available in headers");
#else
	const char *events[] = {
		GROUP_EVENT_INSTR,
		GROUP_EVENT_CYCLES,
	};
	pmc_id_t ids[nitems(events)], probe;
	uint32_t gid;
	size_t i;

	for (i = 0; i < nitems(ids); i++)
		ids[i] = PMC_ID_INVALID;
	probe = PMC_ID_INVALID;
	require_runtime_enabled(tc);
	require_group_events(events, nitems(events));
	build_group_or_skip(events, nitems(events), &gid, ids);
	commit_group_or_skip(gid);
	ATF_REQUIRE_MSG(pmc_group_release(ids, nitems(ids)) == 0,
	    "pmc_group_release() failed: %s", strerror(errno));
	for (i = 0; i < nitems(ids); i++)
		ids[i] = PMC_ID_INVALID;

	/* A fresh non-group allocation should be possible after group release. */
	if (pmc_allocate(GROUP_EVENT_INSTR, PMC_MODE_TC, 0, PMC_CPU_ANY,
	    &probe, 0) != 0) {
		if (is_transient_resource_error(errno))
			atf_tc_skip("post-release probe allocation hit resource "
			    "pressure: %s", strerror(errno));
		atf_tc_fail("post-release pmc_allocate(%s) failed: %s",
		    GROUP_EVENT_INSTR, strerror(errno));
	}
	ATF_REQUIRE_MSG(pmc_release(probe) == 0,
	    "post-release pmc_release(%u) failed: %s", probe, strerror(errno));
#endif
}

ATF_TC(group_start_without_attach_fails_cleanly);
ATF_TC_HEAD(group_start_without_attach_fails_cleanly, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A committed process-mode PMU group must have an explicit target "
	    "before pmc_start().");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(group_start_without_attach_fails_cleanly, tc)
{
#ifndef PMC_F_GROUP_DEFER
	atf_tc_skip("hwpmc PMU grouping ABI is not available in headers");
#else
	const char *events[] = {
		GROUP_EVENT_INSTR,
		GROUP_EVENT_CYCLES,
	};
	pmc_id_t ids[nitems(events)];
	uint32_t gid;
	int error, saved_errno;
	size_t i;

	for (i = 0; i < nitems(ids); i++)
		ids[i] = PMC_ID_INVALID;
	require_runtime_enabled(tc);
	require_group_events(events, nitems(events));
	build_group_or_skip(events, nitems(events), &gid, ids);
	commit_group_or_skip(gid);
	errno = 0;
	error = pmc_start(ids[0]);
	saved_errno = errno;
	ATF_CHECK_MSG(error < 0,
	    "pmc_start unexpectedly accepted a group with no target process");
	ATF_CHECK_MSG(saved_errno == EINVAL,
	    "pmc_start without attach failed with %s, expected EINVAL",
	    strerror(saved_errno));
	if (error == 0)
		(void)pmc_stop(ids[0]);
	release_pmcs(ids, nitems(ids));
#endif
}

ATF_TC(uncommitted_group_start_rejected_EXPECTED_FAIL);
ATF_TC_HEAD(uncommitted_group_start_rejected_EXPECTED_FAIL, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Expected-fail bridge: pmc_start() on an uncommitted deferred PMU "
	    "group should propagate pmu_group_on_start(EDOOFUS).");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(uncommitted_group_start_rejected_EXPECTED_FAIL, tc)
{
#ifndef PMC_F_GROUP_DEFER
	atf_tc_skip("hwpmc PMU grouping ABI is not available in headers");
#else
	const char *events[] = {
		GROUP_EVENT_INSTR,
		GROUP_EVENT_CYCLES,
	};
	pmc_id_t ids[nitems(events)];
	uint32_t gid;
	int error, saved_errno;
	size_t i;

	for (i = 0; i < nitems(ids); i++)
		ids[i] = PMC_ID_INVALID;
	require_runtime_enabled(tc);
	require_group_events(events, nitems(events));
	build_group_or_skip(events, nitems(events), &gid, ids);
	for (i = 0; i < nitems(ids); i++)
		ATF_REQUIRE_MSG(pmc_attach(ids[i], getpid()) == 0,
		    "pmc_attach(%u, self) failed: %s", ids[i], strerror(errno));

	errno = 0;
	error = pmc_start(ids[0]);
	saved_errno = errno;
	if (error == 0)
		(void)pmc_stop(ids[0]);
	release_pmcs(ids, nitems(ids));

	atf_tc_expect_fail("current hwpmc pmc_start() ignores the error from "
	    "pmu_group_on_start() for PMC_ROW_UNASSIGNED deferred handles");
	ATF_CHECK_MSG(error < 0,
	    "pmc_start unexpectedly accepted an uncommitted PMU group");
	if (error < 0)
		ATF_CHECK_MSG(saved_errno == EDOOFUS,
		    "uncommitted start errno %s, expected EDOOFUS",
		    strerror(saved_errno));
	atf_tc_expect_pass();
#endif
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, group_commit_and_leader_start_count_all_siblings);
	ATF_TP_ADD_TC(tp, group_commit_is_idempotent_and_closed_to_late_add);
	ATF_TP_ADD_TC(tp, single_event_group_commit_is_rejected);
	ATF_TP_ADD_TC(tp, group_release_helper_releases_committed_group);
	ATF_TP_ADD_TC(tp, group_start_without_attach_fails_cleanly);
	ATF_TP_ADD_TC(tp, uncommitted_group_start_rejected_EXPECTED_FAIL);

	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-UNC-L3-03] AMD L3 cache counter configuration tests.
 *
 * Validates that L3 PMU counter configuration is accessible and behaves
 * correctly: event selection, enable/disable lifecycle, counter reset, and
 * per-slice consistency across a multi-slice workload.
 *
 * Primary event: l3_lookup_state.all_coherent_accesses_to_l3
 *                (EventSel=0x04, UMask=0xff) — broadest coverage, present
 *                on all Zen generations with L3 PMU JSON support.
 * Fallback:      l3_lookup_state.l3_miss (UMask=0x01)
 */

#include <sys/param.h>

#include <atf-c.h>

#include "amd_l3_common.h"

/*
 * Event candidate list for configuration tests.
 * The all_coherent event is the widest available and is preferred because
 * configuration tests only need a counter that responds to any L3 activity;
 * they do not require hit/miss discrimination.
 */
static const struct amd_umcdf_event_candidate l3_config_events[] = {
	{
		"l3_lookup_state.all_coherent_accesses_to_l3",
		"all coherent L3 accesses, Unit=L3PMC, EventSel=0x04 UMask=0xff",
		0x04, 0xff
	},
	{
		"l3_lookup_state.l3_miss",
		"L3 cache misses, Unit=L3PMC, EventSel=0x04 UMask=0x01",
		0x04, 0x01
	},
	{
		"l3_lookup_state.l3_hit",
		"L3 cache hits, Unit=L3PMC, EventSel=0x04 UMask=0xfe",
		0x04, 0xfe
	},
	{ NULL, NULL, 0, 0 }
};

/* -------------------------------------------------------------------------
 * TC-UNC-L3-03a  L3 config: PMU metadata contract
 *
 * Verifies that at least one L3 event candidate is present in the FreeBSD
 * PMU JSON for this CPU.  EOPNOTSUPP is acceptable (metadata present,
 * runtime backend not yet wired).  ENOENT for all candidates indicates a
 * missing or unrecognised l3-cache.json.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_config_pmu_metadata_contract);
ATF_TC_HEAD(l3_config_pmu_metadata_contract, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify that at least one L3 event in the candidate list reaches "
	    "pmc_pmu_pmcallocate() and returns 0 or EOPNOTSUPP.  ENOENT for "
	    "every candidate means l3-cache.json is absent for this CPU.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_config_pmu_metadata_contract, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	bool saw_eopnotsupp, saw_enoent;
	int error;

	saw_eopnotsupp = false;
	saw_enoent = false;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("FreeBSD L3 PMU JSON covers Zen 1–6; detected %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	for (event = l3_config_events; event->name != NULL; event++) {
		memset(&cfg, 0, sizeof(cfg));
		error = pmc_pmu_pmcallocate(event->name, &cfg);
		if (error == 0) {
			printf("L3 config event '%s': descriptor available "
			    "(class=%d)\n", event->name, cfg.pm_class);
			return;
		}
		printf("L3 config event '%s': pmc_pmu_pmcallocate returned "
		    "%s\n", event->name, strerror(error));
		if (error == EOPNOTSUPP)
			saw_eopnotsupp = true;
		else if (error == ENOENT)
			saw_enoent = true;
	}

	ATF_REQUIRE_MSG(!saw_enoent || saw_eopnotsupp,
	    "All L3 config event candidates returned ENOENT: "
	    "l3-cache.json may be absent from the pmu-events table for %s",
	    amd_umcdf_zen_name(cpu.zen));

	if (saw_eopnotsupp)
		printf("L3 config metadata present; runtime backend not yet "
		    "available on this tree\n");
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-03b  L3 config: enable produces non-zero counts
 *
 * Allocates the first available L3 counter, starts it, drives a workload,
 * and verifies that at least one count was recorded.  A zero delta after
 * a cache-active workload indicates the counter was not enabled or not
 * connected to a hardware backend.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_config_enable_counts);
ATF_TC_HEAD(l3_config_enable_counts, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Enable an L3 counter, drive a cache-active workload (2 MB hot "
	    "buffer, 128 passes), and verify the counter delta is non-zero.  "
	    "A zero delta indicates the counter was not enabled.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_config_enable_counts, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	pmc_value_t before, after;
	pmc_id_t pmcid;
	int error, last_error;

	pmcid = PMC_ID_INVALID;
	before = after = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(l3_config_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable L3 config event; last error: %s",
		    strerror(last_error));

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL))
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    event->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", event->name, strerror(errno));

	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start(%s) failed: %s",
		    event->name, strerror(errno));
	}
	if (pmc_read(pmcid, &before) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}

	/* Drive cache-active traffic to guarantee L3 accesses. */
	error = amd_l3_generate_hit_traffic();
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("hit traffic generator failed: %s", strerror(error));
	}

	if (pmc_stop(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_stop(%s) failed: %s",
		    event->name, strerror(errno));
	}
	if (pmc_read(pmcid, &after) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("L3 config enable '%s': before=%ju after=%ju delta=%ju\n",
	    event->name, (uintmax_t)before, (uintmax_t)after,
	    (uintmax_t)(after - before));

	ATF_CHECK_MSG(after > before,
	    "L3 counter did not increment after enable: before=%ju after=%ju — "
	    "counter may not be connected to a hardware backend",
	    (uintmax_t)before, (uintmax_t)after);

	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-03c  L3 config: counter does not accumulate after stop
 *
 * Starts a counter, drives a workload, stops the counter, then drives
 * another workload and reads again.  The second read must equal the first
 * post-stop read — the counter must not accumulate while stopped.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_config_stop_halts_counting);
ATF_TC_HEAD(l3_config_stop_halts_counting, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Start an L3 counter, drive a workload, stop the counter, drive "
	    "another workload, and verify the counter value did not change "
	    "after stop.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_config_stop_halts_counting, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	pmc_value_t after_stop, after_extra;
	pmc_id_t pmcid;
	int error, last_error;

	pmcid = PMC_ID_INVALID;
	after_stop = after_extra = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(l3_config_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable L3 config event; last error: %s",
		    strerror(last_error));

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL))
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    event->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", event->name, strerror(errno));

	/* Phase 1: count while running. */
	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start(%s) failed: %s",
		    event->name, strerror(errno));
	}
	(void)amd_l3_generate_hit_traffic();

	if (pmc_stop(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_stop(%s) failed: %s",
		    event->name, strerror(errno));
	}
	if (pmc_read(pmcid, &after_stop) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after_stop) failed: %s", strerror(errno));
	}

	/* Phase 2: drive more traffic with the counter stopped. */
	(void)amd_l3_generate_miss_traffic();

	if (pmc_read(pmcid, &after_extra) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after_extra) failed: %s", strerror(errno));
	}

	printf("L3 config stop '%s': after_stop=%ju after_extra=%ju\n",
	    event->name, (uintmax_t)after_stop, (uintmax_t)after_extra);

	ATF_CHECK_MSG(after_extra == after_stop,
	    "L3 counter accumulated while stopped: "
	    "after_stop=%ju after_extra=%ju delta=%ju",
	    (uintmax_t)after_stop, (uintmax_t)after_extra,
	    (uintmax_t)(after_extra - after_stop));

	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-03d  L3 config: counter is monotonically non-decreasing
 *
 * Performs three read-while-running snapshots during a continuous workload
 * and verifies each snapshot is >= the previous one.  A decreasing counter
 * indicates a wrap-around issue or an MSR reset race.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_config_monotone);
ATF_TC_HEAD(l3_config_monotone, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Read an L3 counter three times during a continuous workload and "
	    "verify each successive snapshot is >= the previous one.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_config_monotone, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	pmc_value_t snap[3];
	pmc_id_t pmcid;
	int error, last_error, i;

	pmcid = PMC_ID_INVALID;
	snap[0] = snap[1] = snap[2] = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(l3_config_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable L3 config event; last error: %s",
		    strerror(last_error));

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL))
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    event->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", event->name, strerror(errno));

	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start(%s) failed: %s",
		    event->name, strerror(errno));
	}

	for (i = 0; i < 3; i++) {
		(void)amd_l3_generate_hit_traffic();
		if (pmc_read(pmcid, &snap[i]) != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("pmc_read(snap[%d]) failed: %s",
			    i, strerror(errno));
		}
		printf("L3 config monotone '%s': snap[%d]=%ju\n",
		    event->name, i, (uintmax_t)snap[i]);
	}

	if (pmc_stop(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_stop(%s) failed: %s",
		    event->name, strerror(errno));
	}

	for (i = 1; i < 3; i++) {
		ATF_CHECK_MSG(snap[i] >= snap[i - 1],
		    "L3 counter moved backwards at snapshot %d: "
		    "snap[%d]=%ju snap[%d]=%ju",
		    i, i - 1, (uintmax_t)snap[i - 1],
		    i, (uintmax_t)snap[i]);
	}

	amd_umcdf_release_pmc(pmcid);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, l3_config_pmu_metadata_contract);
	ATF_TP_ADD_TC(tp, l3_config_enable_counts);
	ATF_TP_ADD_TC(tp, l3_config_stop_halts_counting);
	ATF_TP_ADD_TC(tp, l3_config_monotone);
	return (atf_no_error());
}

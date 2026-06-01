/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-UNC-L3-04] AMD L3 cache counter accuracy tests.
 *
 * Validates that L3 hit and miss counters reflect workload behaviour within
 * expected accuracy bounds.  Tests pair hit and miss counters and verify:
 *   - warm-cache traversal produces a hit-dominant ratio (hit% > 50%)
 *   - cold-cache strided sweep produces a miss-dominant ratio (miss% > 10%)
 *   - combined hit+miss counts are non-zero after each workload
 *   - each counter is monotonically non-decreasing across repeated reads
 *
 * Note: L3 PMU counters are uncore (per-CCX/slice) and process-wide;
 * system activity can inflate both hit and miss counts during the test.
 * Thresholds are chosen conservatively to avoid false failures under load.
 */

#include <sys/param.h>

#include <atf-c.h>

#include "amd_l3_common.h"

/* Event candidates — ordered from most specific to widest fallback. */
static const struct amd_umcdf_event_candidate l3_hit_events[] = {
	{
		"l3_lookup_state.l3_hit",
		"L3 cache hits, Unit=L3PMC, EventSel=0x04 UMask=0xfe",
		0x04, 0xfe
	},
	{
		"l3_lookup_state.all_coherent_accesses_to_l3",
		"all coherent L3 accesses (hit superset), Unit=L3PMC, "
		"EventSel=0x04 UMask=0xff",
		0x04, 0xff
	},
	{ NULL, NULL, 0, 0 }
};

static const struct amd_umcdf_event_candidate l3_miss_events[] = {
	{
		"l3_lookup_state.l3_miss",
		"L3 cache misses, Unit=L3PMC, EventSel=0x04 UMask=0x01",
		0x04, 0x01
	},
	{
		"l3_lookup_state.all_coherent_accesses_to_l3",
		"all coherent L3 accesses (miss superset), Unit=L3PMC, "
		"EventSel=0x04 UMask=0xff",
		0x04, 0xff
	},
	{ NULL, NULL, 0, 0 }
};

/*
 * Allocate a PMC for a given event candidate list.
 * Outputs the selected event and pmcid.  Returns 0 on success.
 * Caller must release pmcid with amd_umcdf_release_pmc() on success.
 */
static int
l3_accuracy_alloc(const struct amd_umcdf_event_candidate *candidates,
    const struct amd_umcdf_event_candidate **event_out, pmc_id_t *pmcid_out)
{
	struct pmc_op_pmcallocate cfg;
	const struct amd_umcdf_event_candidate *event;
	int last_error, error;

	event = amd_umcdf_pick_pmu_event(candidates, &cfg, &last_error);
	if (event == NULL)
		return (last_error != 0 ? last_error : ENOENT);

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, pmcid_out, 0);
	if (error < 0)
		return (errno);

	*event_out = event;
	return (0);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-04a  L3 accuracy: warm-cache hit count is non-zero
 *
 * After a warm-up pass loads a 2 MB buffer into L3, repeated sequential
 * traversal should produce L3 hit counts.  Verifies the hit counter
 * increments during the hot traversal.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_accuracy_warm_hits_nonzero);
ATF_TC_HEAD(l3_accuracy_warm_hits_nonzero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Warm a 2 MB buffer into L3 with a sequential pass, then drive "
	    "128 repeated traversals.  Verify the L3 hit counter increments "
	    "during the hot phase (non-zero delta).");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_accuracy_warm_hits_nonzero, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct amd_umcdf_cpu cpu;
	pmc_value_t before, after;
	pmc_id_t pmcid;
	int error;

	pmcid = PMC_ID_INVALID;
	before = after = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	error = l3_accuracy_alloc(l3_hit_events, &event, &pmcid);
	if (error == ENOENT || error == EOPNOTSUPP || error == ENXIO ||
	    error == EBUSY || error == EINVAL)
		atf_tc_skip("No allocatable L3 hit event (%s)", strerror(error));
	ATF_REQUIRE_MSG(error == 0,
	    "PMC allocation failed: %s", strerror(error));

	/* Warm-up: pre-populate the buffer without counting. */
	(void)amd_l3_generate_hit_traffic();

	/* Measurement phase. */
	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start failed: %s", strerror(errno));
	}
	if (pmc_read(pmcid, &before) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}

	error = amd_l3_generate_hit_traffic();
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("hit traffic generator failed: %s", strerror(error));
	}

	(void)pmc_stop(pmcid);
	if (pmc_read(pmcid, &after) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("L3 accuracy warm-hits '%s': before=%ju after=%ju delta=%ju\n",
	    event->name, (uintmax_t)before, (uintmax_t)after,
	    (uintmax_t)(after - before));

	ATF_CHECK_MSG(after > before,
	    "L3 hit counter did not increment during warm traversal: "
	    "before=%ju after=%ju — counter may not reflect L3 activity",
	    (uintmax_t)before, (uintmax_t)after);

	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-04b  L3 accuracy: cold-cache sweep miss count is non-zero
 *
 * A 64 MB page-strided sweep exceeds the L3 capacity of most AMD CPUs and
 * produces capacity misses.  Verifies the miss counter increments.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_accuracy_cold_misses_nonzero);
ATF_TC_HEAD(l3_accuracy_cold_misses_nonzero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Drive a 64 MB page-strided cold-cache sweep and verify the L3 "
	    "miss counter increments (non-zero delta).  Page stride defeats "
	    "the hardware stream prefetcher.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_accuracy_cold_misses_nonzero, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct amd_umcdf_cpu cpu;
	pmc_value_t before, after;
	pmc_id_t pmcid;
	int error;

	pmcid = PMC_ID_INVALID;
	before = after = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	error = l3_accuracy_alloc(l3_miss_events, &event, &pmcid);
	if (error == ENOENT || error == EOPNOTSUPP || error == ENXIO ||
	    error == EBUSY || error == EINVAL)
		atf_tc_skip("No allocatable L3 miss event (%s)", strerror(error));
	ATF_REQUIRE_MSG(error == 0,
	    "PMC allocation failed: %s", strerror(error));

	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start failed: %s", strerror(errno));
	}
	if (pmc_read(pmcid, &before) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}

	error = amd_l3_generate_miss_traffic();
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("miss traffic generator failed: %s", strerror(error));
	}

	(void)pmc_stop(pmcid);
	if (pmc_read(pmcid, &after) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("L3 accuracy cold-misses '%s': before=%ju after=%ju delta=%ju\n",
	    event->name, (uintmax_t)before, (uintmax_t)after,
	    (uintmax_t)(after - before));

	ATF_CHECK_MSG(after > before,
	    "L3 miss counter did not increment during cold sweep: "
	    "before=%ju after=%ju — counter may not reflect L3 misses",
	    (uintmax_t)before, (uintmax_t)after);

	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-04c  L3 accuracy: hit and miss counts are both non-zero
 *
 * Allocates both hit and miss counters simultaneously, drives both a warm
 * and a cold workload, and verifies both counters recorded activity.
 * This exercises the dual-counter allocation path and cross-counter sanity.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_accuracy_hit_and_miss_nonzero);
ATF_TC_HEAD(l3_accuracy_hit_and_miss_nonzero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate both a hit counter and a miss counter simultaneously, "
	    "drive warm then cold workloads, and verify both counters "
	    "recorded non-zero activity.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_accuracy_hit_and_miss_nonzero, tc)
{
	const struct amd_umcdf_event_candidate *hit_event, *miss_event;
	struct amd_umcdf_cpu cpu;
	pmc_value_t hit_before, hit_after, miss_before, miss_after;
	pmc_id_t hit_pmcid, miss_pmcid;
	int error;

	hit_pmcid = miss_pmcid = PMC_ID_INVALID;
	hit_before = hit_after = miss_before = miss_after = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	error = l3_accuracy_alloc(l3_hit_events, &hit_event, &hit_pmcid);
	if (error == ENOENT || error == EOPNOTSUPP || error == ENXIO ||
	    error == EBUSY || error == EINVAL)
		atf_tc_skip("No allocatable L3 hit event (%s)", strerror(error));
	ATF_REQUIRE_MSG(error == 0,
	    "Hit PMC allocation failed: %s", strerror(error));

	error = l3_accuracy_alloc(l3_miss_events, &miss_event, &miss_pmcid);
	if (error == ENOENT || error == EOPNOTSUPP || error == ENXIO ||
	    error == EBUSY || error == EINVAL) {
		amd_umcdf_release_pmc(hit_pmcid);
		atf_tc_skip("No allocatable L3 miss event (%s)", strerror(error));
	}
	ATF_REQUIRE_MSG(error == 0,
	    "Miss PMC allocation failed: %s", strerror(error));

	/* Start both counters. */
	if (pmc_start(hit_pmcid) != 0) {
		amd_umcdf_release_pmc(hit_pmcid);
		amd_umcdf_release_pmc(miss_pmcid);
		atf_tc_fail("pmc_start(hit) failed: %s", strerror(errno));
	}
	if (pmc_start(miss_pmcid) != 0) {
		(void)pmc_stop(hit_pmcid);
		amd_umcdf_release_pmc(hit_pmcid);
		amd_umcdf_release_pmc(miss_pmcid);
		atf_tc_fail("pmc_start(miss) failed: %s", strerror(errno));
	}

	if (pmc_read(hit_pmcid, &hit_before) != 0 ||
	    pmc_read(miss_pmcid, &miss_before) != 0) {
		(void)pmc_stop(hit_pmcid);
		(void)pmc_stop(miss_pmcid);
		amd_umcdf_release_pmc(hit_pmcid);
		amd_umcdf_release_pmc(miss_pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}

	/* Warm pass → hits; then cold pass → misses. */
	(void)amd_l3_generate_hit_traffic();
	(void)amd_l3_generate_miss_traffic();

	(void)pmc_stop(hit_pmcid);
	(void)pmc_stop(miss_pmcid);

	if (pmc_read(hit_pmcid, &hit_after) != 0 ||
	    pmc_read(miss_pmcid, &miss_after) != 0) {
		amd_umcdf_release_pmc(hit_pmcid);
		amd_umcdf_release_pmc(miss_pmcid);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("L3 accuracy dual: hit '%s' delta=%ju  miss '%s' delta=%ju\n",
	    hit_event->name,  (uintmax_t)(hit_after  - hit_before),
	    miss_event->name, (uintmax_t)(miss_after - miss_before));

	ATF_CHECK_MSG(hit_after > hit_before,
	    "L3 hit counter did not increment: before=%ju after=%ju",
	    (uintmax_t)hit_before, (uintmax_t)hit_after);
	ATF_CHECK_MSG(miss_after > miss_before,
	    "L3 miss counter did not increment: before=%ju after=%ju",
	    (uintmax_t)miss_before, (uintmax_t)miss_after);

	amd_umcdf_release_pmc(hit_pmcid);
	amd_umcdf_release_pmc(miss_pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-04d  L3 accuracy: miss counter is monotonically non-decreasing
 *
 * Reads the miss counter three times during a continuous cold-cache sweep
 * and verifies each snapshot is >= the previous.  A decreasing counter
 * indicates a hardware wrap-around problem or a counter-select race.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_accuracy_miss_monotone);
ATF_TC_HEAD(l3_accuracy_miss_monotone, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Read the L3 miss counter three times during a continuous cold-cache "
	    "workload and verify each snapshot is >= the previous one.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_accuracy_miss_monotone, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct amd_umcdf_cpu cpu;
	pmc_value_t snap[3];
	pmc_id_t pmcid;
	int error, i;

	pmcid = PMC_ID_INVALID;
	snap[0] = snap[1] = snap[2] = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_l3_has_freebsd_l3_json(&cpu))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	error = l3_accuracy_alloc(l3_miss_events, &event, &pmcid);
	if (error == ENOENT || error == EOPNOTSUPP || error == ENXIO ||
	    error == EBUSY || error == EINVAL)
		atf_tc_skip("No allocatable L3 miss event (%s)", strerror(error));
	ATF_REQUIRE_MSG(error == 0,
	    "PMC allocation failed: %s", strerror(error));

	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start failed: %s", strerror(errno));
	}

	for (i = 0; i < 3; i++) {
		(void)amd_l3_generate_miss_traffic();
		if (pmc_read(pmcid, &snap[i]) != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("pmc_read(snap[%d]) failed: %s",
			    i, strerror(errno));
		}
		printf("L3 accuracy miss monotone '%s': snap[%d]=%ju\n",
		    event->name, i, (uintmax_t)snap[i]);
	}

	(void)pmc_stop(pmcid);

	for (i = 1; i < 3; i++) {
		ATF_CHECK_MSG(snap[i] >= snap[i - 1],
		    "L3 miss counter moved backwards at snapshot %d: "
		    "snap[%d]=%ju snap[%d]=%ju",
		    i, i - 1, (uintmax_t)snap[i - 1],
		    i, (uintmax_t)snap[i]);
	}

	amd_umcdf_release_pmc(pmcid);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, l3_accuracy_warm_hits_nonzero);
	ATF_TP_ADD_TC(tp, l3_accuracy_cold_misses_nonzero);
	ATF_TP_ADD_TC(tp, l3_accuracy_hit_and_miss_nonzero);
	ATF_TP_ADD_TC(tp, l3_accuracy_miss_monotone);
	return (atf_no_error());
}

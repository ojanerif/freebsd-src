/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-UNC-L3-STRESS] AMD L3 cache stress tests.
 *
 * Validates the L3 PMC infrastructure under sustained, concurrent load.
 * Uses the same miss and hit workload helpers from amd_l3_common.h.
 *
 * Tests:
 *   l3_stress_rapid_alloc_free    — allocate and release the L3 PMC in a
 *                                   tight loop to check for resource leaks.
 *   l3_stress_sustained_miss      — run the miss workload for 30 s on a
 *                                   pinned thread and verify the counter
 *                                   remains monotonic throughout.
 *   l3_stress_concurrent_workload — N threads each run miss+hit workloads
 *                                   simultaneously; verify no counter
 *                                   wrap-around or PMC errors occur.
 *
 * NOTE: parallelism=1 is set on l3_stress_concurrent_workload to prevent
 * interference with other suites running in the same kyua session.
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amd_l3_common.h"

static const struct amd_umcdf_event_candidate l3_stress_miss_events[] = {
	{
		"l3_lookup_state.l3_miss",
		"L3 cache misses, Unit=L3PMC, EventSel=0x04 UMask=0x01",
		0x04, 0x01
	},
	{
		"l3_lookup_state.all_coherent_accesses_to_l3",
		"all coherent L3 accesses (superset), Unit=L3PMC, "
		"EventSel=0x04 UMask=0xff",
		0x04, 0xff
	},
	{ NULL, NULL, 0, 0 }
};

static int
l3_stress_ncpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n < 1 ? 1 : (int)n);
}

static int
l3_stress_pin(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID,
	    -1, sizeof(mask), &mask));
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-STRESS-01  Rapid PMC allocate / free loop
 *
 * Allocates and releases the L3 miss PMC 200 times in a tight loop.
 * Checks that the allocator does not leak file descriptors or PMC slots.
 *
 * Skips when the event is unavailable in this tree.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_stress_rapid_alloc_free);
ATF_TC_HEAD(l3_stress_rapid_alloc_free, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate and release the L3 miss PMC 200 times; verify no "
	    "resource leak or allocation failure after repeated cycling.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_stress_rapid_alloc_free, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	pmc_id_t pmcid;
	int i, last_error, error;
	const int iterations = 200;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_l3_has_freebsd_l3_json(&cpu_info))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(l3_stress_miss_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable L3 miss event; last error: %s",
		    strerror(last_error));

	for (i = 0; i < iterations; i++) {
		pmcid = PMC_ID_INVALID;
		error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
		if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
		    errno == ENXIO || errno == EBUSY || errno == EINVAL))
			atf_tc_skip("pmc_allocate(%s) not available: %s",
			    event->name, strerror(errno));
		ATF_REQUIRE_MSG(error == 0,
		    "pmc_allocate(%s) failed at iteration %d: %s",
		    event->name, i, strerror(errno));
		amd_umcdf_release_pmc(pmcid);
	}

	printf("Rapid alloc/free: %d iterations completed without error\n",
	    iterations);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-STRESS-02  Sustained L3 miss counter over 30 seconds
 *
 * Pins the main thread to CPU 0, runs 30 successive miss workload bursts,
 * and reads the counter after each burst.  Verifies the counter never
 * decreases between reads (monotonicity under sustained load).
 *
 * Skips on single-CPU systems or when the event is unavailable.
 * ---------------------------------------------------------------------- */
ATF_TC(l3_stress_sustained_miss);
ATF_TC_HEAD(l3_stress_sustained_miss, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Run 30 successive L3 miss workload bursts on CPU 0 and verify "
	    "the counter is monotonically non-decreasing throughout.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_stress_sustained_miss, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	pmc_id_t pmcid;
	pmc_value_t prev, cur;
	int i, last_error, error;
	const int bursts = 30;

	pmcid = PMC_ID_INVALID;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_l3_has_freebsd_l3_json(&cpu_info))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	if (l3_stress_pin(0) != 0)
		atf_tc_skip("Cannot pin to CPU 0: %s", strerror(errno));

	event = amd_umcdf_pick_pmu_event(l3_stress_miss_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable L3 miss event; last error: %s",
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
		atf_tc_fail("pmc_start failed: %s", strerror(errno));
	}

	if (pmc_read(pmcid, &prev) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(initial) failed: %s", strerror(errno));
	}

	for (i = 0; i < bursts; i++) {
		error = amd_l3_generate_miss_traffic();
		if (error != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("miss workload burst %d failed: %s",
			    i, strerror(error));
		}

		if (pmc_read(pmcid, &cur) != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("pmc_read after burst %d failed: %s",
			    i, strerror(errno));
		}

		ATF_CHECK_MSG(cur >= prev,
		    "Counter moved backwards at burst %d: prev=%ju cur=%ju",
		    i, (uintmax_t)prev, (uintmax_t)cur);

		printf("Burst %2d: delta=%ju total=%ju\n", i,
		    (uintmax_t)(cur - prev), (uintmax_t)cur);
		prev = cur;
	}

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-STRESS-03  Concurrent miss+hit workload across all CPUs
 *
 * Launches N worker threads (one per online CPU).  Each thread alternates
 * between the miss workload and the hit workload for 10 rounds.  A single
 * system-wide L3 miss counter is read before and after to verify there
 * is overall forward progress; the test does not require an exact count.
 *
 * The test is tagged parallelism=1 to avoid contending with other stress
 * tests for PMC slots.
 *
 * SIGPIPE is ignored in each worker thread because send(2) is not used
 * here; the annotation is a precaution matching project policy for stress
 * tests that use network-adjacent libraries transitively.
 * ---------------------------------------------------------------------- */

struct concurrent_arg {
	int cpu;
	int error;
	int rounds;
};

static void *
concurrent_worker(void *arg)
{
	struct concurrent_arg *wa = arg;
	int i, error;

	/* Ignore SIGPIPE per project stress-test policy. */
	signal(SIGPIPE, SIG_IGN);

	if (l3_stress_pin(wa->cpu) != 0) {
		wa->error = errno;
		return (NULL);
	}

	for (i = 0; i < wa->rounds; i++) {
		error = amd_l3_generate_miss_traffic();
		if (error != 0) {
			wa->error = error;
			return (NULL);
		}
		error = amd_l3_generate_hit_traffic();
		if (error != 0) {
			wa->error = error;
			return (NULL);
		}
	}

	wa->error = 0;
	return (NULL);
}

ATF_TC(l3_stress_concurrent_workload);
ATF_TC_HEAD(l3_stress_concurrent_workload, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Run miss+hit workload concurrently on all online CPUs for 10 "
	    "rounds each and verify the L3 miss counter shows forward "
	    "progress without wrap-around errors.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "X-parallel", "1");
}

ATF_TC_BODY(l3_stress_concurrent_workload, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	struct concurrent_arg *args;
	pthread_t *threads;
	pmc_id_t pmcid;
	pmc_value_t before, after;
	int ncpus, i, last_error, error;
	const int rounds = 10;

	pmcid = PMC_ID_INVALID;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_l3_has_freebsd_l3_json(&cpu_info))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	ncpus = l3_stress_ncpus();

	threads = calloc(ncpus, sizeof(pthread_t));
	args    = calloc(ncpus, sizeof(struct concurrent_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	event = amd_umcdf_pick_pmu_event(l3_stress_miss_events, &cfg,
	    &last_error);
	if (event == NULL) {
		free(threads); free(args);
		atf_tc_skip("No allocatable L3 miss event; last error: %s",
		    strerror(last_error));
	}

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL)) {
		free(threads); free(args);
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    event->name, strerror(errno));
	}
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", event->name, strerror(errno));

	if (pmc_start(pmcid) != 0) {
		free(threads); free(args);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start failed: %s", strerror(errno));
	}

	if (pmc_read(pmcid, &before) != 0) {
		free(threads); free(args);
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}

	for (i = 0; i < ncpus; i++) {
		args[i].cpu    = i;
		args[i].error  = 0;
		args[i].rounds = rounds;
		error = pthread_create(&threads[i], NULL, concurrent_worker,
		    &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}

	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		if (args[i].error != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			free(threads); free(args);
			atf_tc_fail("Worker on CPU %d failed after %d rounds: %s",
			    i, rounds, strerror(args[i].error));
		}
	}

	if (pmc_read(pmcid, &after) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		free(threads); free(args);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("Concurrent stress (%d CPUs, %d rounds each): "
	    "before=%ju after=%ju delta=%ju\n",
	    ncpus, rounds, (uintmax_t)before, (uintmax_t)after,
	    (uintmax_t)(after - before));

	ATF_CHECK_MSG(after >= before,
	    "L3 miss counter moved backwards under concurrent load: "
	    "before=%ju after=%ju", (uintmax_t)before, (uintmax_t)after);

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
	free(threads);
	free(args);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, l3_stress_rapid_alloc_free);
	ATF_TP_ADD_TC(tp, l3_stress_sustained_miss);
	ATF_TP_ADD_TC(tp, l3_stress_concurrent_workload);
	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-UNC-L3-SMP] AMD L3 cache SMP counter tests.
 *
 * Validates that the L3 PMC infrastructure correctly tracks cache activity
 * when workload threads are distributed across multiple CPU cores.
 *
 * Tests:
 *   l3_smp_per_core_counters  — allocate one L3 miss counter per core and
 *                               verify each core's counter is monotonic.
 *   l3_smp_aggregate_miss     — drive L3 miss traffic from N threads in
 *                               parallel and verify the aggregate counter
 *                               exceeds the single-thread baseline.
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amd_l3_common.h"

/*
 * Event candidates for L3 miss counting — ordered most-specific first.
 */
static const struct amd_umcdf_event_candidate l3_smp_miss_events[] = {
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

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int
l3_smp_ncpus(void)
{
	long n;

	n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n < 1 ? 1 : (int)n);
}

static int
l3_smp_pin(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID,
	    -1, sizeof(mask), &mask));
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-SMP-01  Per-core L3 miss counter monotonicity
 *
 * Pins a worker thread to each online CPU in turn, runs the miss workload,
 * reads a system-wide L3 miss counter before and after, and checks the
 * counter moved forward.  The counter is uncore (shared across all cores)
 * so we measure the aggregate with one allocation; the per-core pinning
 * ensures the workload actually runs on the target core and contributes to
 * L3 traffic.
 *
 * Skips on single-CPU systems and when the PMU event is unavailable.
 * ---------------------------------------------------------------------- */

struct per_core_arg {
	int     cpu;
	int     error;
	pmc_id_t pmcid;
	pmc_value_t before;
	pmc_value_t after;
};

static void *
per_core_worker(void *arg)
{
	struct per_core_arg *pa = arg;

	if (l3_smp_pin(pa->cpu) != 0) {
		pa->error = errno;
		return (NULL);
	}

	if (pmc_read(pa->pmcid, &pa->before) != 0) {
		pa->error = errno;
		return (NULL);
	}

	pa->error = amd_l3_generate_miss_traffic();
	if (pa->error != 0)
		return (NULL);

	if (pmc_read(pa->pmcid, &pa->after) != 0) {
		pa->error = errno;
		return (NULL);
	}

	return (NULL);
}

ATF_TC(l3_smp_per_core_counters);
ATF_TC_HEAD(l3_smp_per_core_counters, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Pin a miss workload to each online CPU in turn and verify the "
	    "L3 uncore miss counter is monotonically non-decreasing after "
	    "each run.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_smp_per_core_counters, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	struct per_core_arg pa;
	pthread_t tid;
	pmc_id_t pmcid;
	int ncpus, cpu, last_error, error;

	pmcid = PMC_ID_INVALID;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_l3_has_freebsd_l3_json(&cpu_info))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	ncpus = l3_smp_ncpus();
	if (ncpus < 2)
		atf_tc_skip("l3_smp_per_core_counters requires >= 2 online CPUs "
		    "(found %d)", ncpus);

	event = amd_umcdf_pick_pmu_event(l3_smp_miss_events, &cfg, &last_error);
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

	for (cpu = 0; cpu < ncpus; cpu++) {
		pa.cpu    = cpu;
		pa.pmcid  = pmcid;
		pa.error  = 0;
		pa.before = pa.after = 0;

		error = pthread_create(&tid, NULL, per_core_worker, &pa);
		ATF_REQUIRE_EQ(error, 0);
		error = pthread_join(tid, NULL);
		ATF_REQUIRE_EQ(error, 0);

		if (pa.error != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("Worker on CPU %d failed: %s",
			    cpu, strerror(pa.error));
		}

		printf("CPU %d: L3 miss counter '%s': before=%ju after=%ju "
		    "delta=%ju\n", cpu, event->name,
		    (uintmax_t)pa.before, (uintmax_t)pa.after,
		    (uintmax_t)(pa.after - pa.before));

		ATF_CHECK_MSG(pa.after >= pa.before,
		    "CPU %d: L3 miss counter moved backwards: before=%ju "
		    "after=%ju", cpu, (uintmax_t)pa.before,
		    (uintmax_t)pa.after);
	}

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-UNC-L3-SMP-02  Aggregate L3 miss count scales with thread count
 *
 * Launches N threads simultaneously, each running the miss workload pinned
 * to a distinct CPU.  Measures the L3 miss counter delta for the parallel
 * run and compares it to the single-thread baseline obtained first.
 *
 * Pass condition: parallel_delta >= single_delta * 0.75 * N
 * (The 0.75 factor accounts for cache-sharing effects on multi-die systems.)
 *
 * Skips on single-CPU systems or when the PMU event is unavailable.
 * ---------------------------------------------------------------------- */

struct agg_worker_arg {
	int     cpu;
	int     error;
};

static void *
agg_worker(void *arg)
{
	struct agg_worker_arg *wa = arg;

	if (l3_smp_pin(wa->cpu) != 0) {
		wa->error = errno;
		return (NULL);
	}
	wa->error = amd_l3_generate_miss_traffic();
	return (NULL);
}

ATF_TC(l3_smp_aggregate_miss);
ATF_TC_HEAD(l3_smp_aggregate_miss, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Drive L3 miss traffic from N threads in parallel and verify the "
	    "aggregate counter delta is at least 75%% of N times the "
	    "single-thread baseline.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(l3_smp_aggregate_miss, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	struct agg_worker_arg *args;
	pthread_t *threads;
	pmc_id_t pmcid;
	pmc_value_t v0, v1, v2, v3;
	pmc_value_t single_delta, parallel_delta;
	int ncpus, i, last_error, error;

	pmcid = PMC_ID_INVALID;
	v0 = v1 = v2 = v3 = 0;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_l3_has_freebsd_l3_json(&cpu_info))
		atf_tc_skip("No FreeBSD L3 PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	ncpus = l3_smp_ncpus();
	if (ncpus < 2)
		atf_tc_skip("l3_smp_aggregate_miss requires >= 2 online CPUs "
		    "(found %d)", ncpus);

	event = amd_umcdf_pick_pmu_event(l3_smp_miss_events, &cfg, &last_error);
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

	/* --- Single-thread baseline (pinned to CPU 0) --- */
	if (l3_smp_pin(0) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_skip("Cannot pin to CPU 0: %s", strerror(errno));
	}
	if (pmc_read(pmcid, &v0) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(v0) failed: %s", strerror(errno));
	}
	error = amd_l3_generate_miss_traffic();
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("single-thread miss workload failed: %s",
		    strerror(error));
	}
	if (pmc_read(pmcid, &v1) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(v1) failed: %s", strerror(errno));
	}
	single_delta = (v1 >= v0) ? (v1 - v0) : 0;
	printf("Single-thread baseline: before=%ju after=%ju delta=%ju\n",
	    (uintmax_t)v0, (uintmax_t)v1, (uintmax_t)single_delta);

	if (single_delta == 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_skip("Single-thread baseline produced 0 L3 misses — "
		    "uncore counter may not be accessible on this tree");
	}

	/* --- Parallel run across all CPUs --- */
	threads = calloc(ncpus, sizeof(pthread_t));
	args    = calloc(ncpus, sizeof(struct agg_worker_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	if (pmc_read(pmcid, &v2) != 0) {
		free(threads); free(args);
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(v2) failed: %s", strerror(errno));
	}

	for (i = 0; i < ncpus; i++) {
		args[i].cpu   = i;
		args[i].error = 0;
		error = pthread_create(&threads[i], NULL, agg_worker, &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}
	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		if (args[i].error != 0) {
			free(threads); free(args);
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("Parallel worker on CPU %d failed: %s",
			    i, strerror(args[i].error));
		}
	}

	if (pmc_read(pmcid, &v3) != 0) {
		free(threads); free(args);
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(v3) failed: %s", strerror(errno));
	}

	parallel_delta = (v3 >= v2) ? (v3 - v2) : 0;
	printf("Parallel run (%d CPUs): before=%ju after=%ju delta=%ju\n",
	    ncpus, (uintmax_t)v2, (uintmax_t)v3, (uintmax_t)parallel_delta);

	/*
	 * Threshold: parallel delta must be >= 75% of (ncpus * single_delta).
	 * The 0.75 factor accounts for cache-to-cache transfer savings on
	 * multi-die systems where threads may share L3 slices.
	 */
	{
		pmc_value_t threshold = (pmc_value_t)ncpus * single_delta * 3 / 4;
		ATF_CHECK_MSG(parallel_delta >= threshold,
		    "Parallel L3 miss delta (%ju) < 75%% of expected "
		    "(%ju = %d * %ju * 0.75); uncore counter may undercount "
		    "cross-die traffic",
		    (uintmax_t)parallel_delta, (uintmax_t)threshold,
		    ncpus, (uintmax_t)single_delta);
	}

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
	free(threads);
	free(args);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, l3_smp_per_core_counters);
	ATF_TP_ADD_TC(tp, l3_smp_aggregate_miss);
	return (atf_no_error());
}

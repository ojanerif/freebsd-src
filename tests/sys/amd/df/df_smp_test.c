/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-SMP-DF] AMD Data Fabric PMU SMP counter tests.
 *
 * Validates that the DF PMC infrastructure correctly tracks fabric traffic
 * when workload threads are distributed across multiple CPU cores.
 *
 * Tests:
 *   df_smp_per_core_counters  — run DF workload pinned to each online CPU
 *                               in turn; verify the domain-wide counter is
 *                               monotonically non-decreasing after each run.
 *   df_smp_aggregate_traffic  — drive DF traffic from N threads in parallel
 *                               and verify the aggregate counter delta
 *                               exceeds the single-thread baseline.
 *
 * Jira: FreeBSD-Tests-026
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

#include "amd_df_common.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int
df_smp_ncpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n < 1 ? 1 : (int)n);
}

static int
df_smp_pin(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID,
	    -1, sizeof(mask), &mask));
}

/* -------------------------------------------------------------------------
 * TC-SMP-DF-01  df_smp_per_core_counters
 *
 * Pins a worker thread to each online CPU in turn, runs the remote-traffic
 * workload, reads the domain-wide DF PMC before and after, and checks the
 * counter is non-decreasing.  DF counters are domain-wide (shared across
 * all cores on a die), so the monotonicity check holds regardless of which
 * core the workload runs on.
 *
 * Skips on single-CPU systems and when the PMU event is unavailable.
 * ---------------------------------------------------------------------- */

struct df_per_core_arg {
	int		cpu;
	int		error;
	pmc_id_t	pmcid;
	pmc_value_t	before;
	pmc_value_t	after;
};

static void *
df_per_core_worker(void *arg)
{
	struct df_per_core_arg *pa = arg;

	if (df_smp_pin(pa->cpu) != 0) {
		pa->error = errno;
		return (NULL);
	}
	if (pmc_read(pa->pmcid, &pa->before) != 0) {
		pa->error = errno;
		return (NULL);
	}
	pa->error = amd_df_generate_remote_traffic();
	if (pa->error != 0)
		return (NULL);
	if (pmc_read(pa->pmcid, &pa->after) != 0) {
		pa->error = errno;
		return (NULL);
	}
	return (NULL);
}

ATF_TC(df_smp_per_core_counters);
ATF_TC_HEAD(df_smp_per_core_counters, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Pin a DF traffic workload to each online CPU in turn and verify "
	    "the domain-wide DF counter is monotonically non-decreasing after "
	    "each run.  Skips on single-CPU systems.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_smp_per_core_counters, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	struct df_per_core_arg pa;
	pthread_t tid;
	pmc_id_t pmcid;
	int ncpus, cpu, last_error, error;

	pmcid = PMC_ID_INVALID;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_umcdf_has_df_feature(&cpu_info))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu_info))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	ncpus = df_smp_ncpus();
	if (ncpus < 2)
		atf_tc_skip("df_smp_per_core_counters requires >= 2 online "
		    "CPUs (found %d)", ncpus);

	event = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable DF DRAM event; last error: %s",
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

		error = pthread_create(&tid, NULL, df_per_core_worker, &pa);
		ATF_REQUIRE_EQ(error, 0);
		pthread_join(tid, NULL);

		if (pa.error != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("Worker on CPU %d failed: %s",
			    cpu, strerror(pa.error));
		}

		printf("CPU %d [%s]: before=%ju after=%ju delta=%ju\n",
		    cpu, event->name,
		    (uintmax_t)pa.before, (uintmax_t)pa.after,
		    (uintmax_t)(pa.after - pa.before));

		ATF_CHECK_MSG(pa.after >= pa.before,
		    "CPU %d: DF counter moved backwards: before=%ju after=%ju",
		    cpu, (uintmax_t)pa.before, (uintmax_t)pa.after);
	}

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-SMP-DF-02  df_smp_aggregate_traffic
 *
 * Launches N threads simultaneously (one per online CPU), each running
 * the remote-traffic workload.  Measures the domain-wide DF counter delta
 * for the parallel run and compares it against the single-thread baseline.
 *
 * Pass condition: parallel_delta >= single_delta * 0.5 * N
 * A 0.5 factor (vs 0.75 in L3) accounts for the fact that DF counters
 * are domain-wide and may not scale linearly with thread count on
 * multi-die systems where traffic aggregates across fabric links.
 *
 * Skips on single-CPU systems or when the PMU event is unavailable.
 * ---------------------------------------------------------------------- */

struct df_agg_worker_arg {
	int	cpu;
	int	error;
};

static void *
df_agg_worker(void *arg)
{
	struct df_agg_worker_arg *wa = arg;

	if (df_smp_pin(wa->cpu) != 0) {
		wa->error = errno;
		return (NULL);
	}
	wa->error = amd_df_generate_remote_traffic();
	return (NULL);
}

ATF_TC(df_smp_aggregate_traffic);
ATF_TC_HEAD(df_smp_aggregate_traffic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Drive DF DRAM traffic from N threads in parallel and verify the "
	    "aggregate domain-wide counter delta is at least 50%% of N times "
	    "the single-thread baseline.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_smp_aggregate_traffic, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	struct df_agg_worker_arg *args;
	pthread_t *threads;
	pmc_id_t pmcid;
	pmc_value_t v0, v1, v2, v3;
	pmc_value_t single_delta, parallel_delta;
	int ncpus, i, last_error, error;

	pmcid = PMC_ID_INVALID;
	v0 = v1 = v2 = v3 = 0;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_umcdf_has_df_feature(&cpu_info))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu_info))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	ncpus = df_smp_ncpus();
	if (ncpus < 2)
		atf_tc_skip("df_smp_aggregate_traffic requires >= 2 online "
		    "CPUs (found %d)", ncpus);

	event = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable DF DRAM event; last error: %s",
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

	/* Single-thread baseline pinned to CPU 0. */
	if (df_smp_pin(0) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_skip("Cannot pin to CPU 0: %s", strerror(errno));
	}
	if (pmc_read(pmcid, &v0) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(v0) failed: %s", strerror(errno));
	}
	error = amd_df_generate_remote_traffic();
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("single-thread workload failed: %s", strerror(error));
	}
	if (pmc_read(pmcid, &v1) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(v1) failed: %s", strerror(errno));
	}
	single_delta = (v1 >= v0) ? (v1 - v0) : 0;
	printf("Single-thread baseline [%s]: before=%ju after=%ju delta=%ju\n",
	    event->name, (uintmax_t)v0, (uintmax_t)v1, (uintmax_t)single_delta);

	if (single_delta == 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_skip("Single-thread baseline produced 0 DF events — "
		    "domain-wide counter may not be accessible on this tree");
	}

	/* Parallel run. */
	threads = calloc(ncpus, sizeof(pthread_t));
	args    = calloc(ncpus, sizeof(struct df_agg_worker_arg));
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
		error = pthread_create(&threads[i], NULL, df_agg_worker,
		    &args[i]);
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
	printf("Parallel run (%d CPUs) [%s]: before=%ju after=%ju delta=%ju\n",
	    ncpus, event->name,
	    (uintmax_t)v2, (uintmax_t)v3, (uintmax_t)parallel_delta);

	/*
	 * Threshold: 50% of (ncpus * single_delta).
	 * Lower than L3 (75%) because DF counters are domain-wide and traffic
	 * from multiple dies may share fabric links, capping the aggregate.
	 */
	{
		pmc_value_t threshold =
		    (pmc_value_t)ncpus * single_delta / 2;
		ATF_CHECK_MSG(parallel_delta >= threshold,
		    "Parallel DF delta (%ju) < 50%% of expected "
		    "(%ju = %d * %ju * 0.5); DF counter may undercount "
		    "multi-die traffic",
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
	ATF_TP_ADD_TC(tp, df_smp_per_core_counters);
	ATF_TP_ADD_TC(tp, df_smp_aggregate_traffic);
	return (atf_no_error());
}

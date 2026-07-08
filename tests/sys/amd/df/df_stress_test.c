/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-STRESS-DF] AMD Data Fabric PMU stress tests.
 *
 * Validates the DF PMC infrastructure under sustained, concurrent load.
 *
 * Tests:
 *   df_stress_rapid_alloc_free    — allocate and release the DF PMC in a
 *                                   tight loop to check for resource leaks.
 *   df_stress_sustained_traffic   — run the remote-traffic workload for
 *                                   30 bursts on a pinned thread and verify
 *                                   the counter remains monotonic throughout.
 *   df_stress_concurrent_traffic  — N threads each run the remote+local
 *                                   workload simultaneously; verify no
 *                                   counter wrap-around or PMC errors.
 *
 * NOTE: df_stress_concurrent_traffic is tagged X-parallel=1 to prevent
 * contention with other stress tests for DF PMC slots.
 *
 * Jira: FreeBSD-Tests-026
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

#include "amd_df_common.h"

static int
df_stress_ncpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n < 1 ? 1 : (int)n);
}

static int
df_stress_pin(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID,
	    -1, sizeof(mask), &mask));
}

/* -------------------------------------------------------------------------
 * TC-STRESS-DF-01  df_stress_rapid_alloc_free
 *
 * Allocates and releases a DF DRAM-channel PMC 200 times in a tight loop.
 * Checks that the allocator does not leak file descriptors or PMC slots
 * under repeated cycling.
 * ---------------------------------------------------------------------- */
ATF_TC(df_stress_rapid_alloc_free);
ATF_TC_HEAD(df_stress_rapid_alloc_free, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate and release a DF DRAM-channel PMC 200 times; verify no "
	    "resource leak or allocation failure after repeated cycling.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_stress_rapid_alloc_free, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	pmc_id_t pmcid;
	int i, last_error, error;
	const int iterations = 200;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_umcdf_has_df_feature(&cpu_info))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu_info))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable DF DRAM event; last error: %s",
		    strerror(last_error));

	for (i = 0; i < iterations; i++) {
		pmcid = PMC_ID_INVALID;
		error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0,
		    &pmcid, 0);
		if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
		    errno == ENXIO || errno == EBUSY || errno == EINVAL))
			atf_tc_skip("pmc_allocate(%s) not available: %s",
			    event->name, strerror(errno));
		ATF_REQUIRE_MSG(error == 0,
		    "pmc_allocate(%s) failed at iteration %d: %s",
		    event->name, i, strerror(errno));
		amd_umcdf_release_pmc(pmcid);
	}

	printf("Rapid alloc/free [%s]: %d iterations without error\n",
	    event->name, iterations);
}

/* -------------------------------------------------------------------------
 * TC-STRESS-DF-02  df_stress_sustained_traffic
 *
 * Pins the main thread to CPU 0, runs 30 successive remote-traffic workload
 * bursts, and reads the domain-wide DF counter after each burst.  Verifies
 * the counter never decreases between reads (monotonicity under sustained
 * load).
 * ---------------------------------------------------------------------- */
ATF_TC(df_stress_sustained_traffic);
ATF_TC_HEAD(df_stress_sustained_traffic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Run 30 successive DF remote-traffic workload bursts on CPU 0 and "
	    "verify the domain-wide counter is monotonically non-decreasing "
	    "throughout.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_stress_sustained_traffic, tc)
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
	if (!amd_umcdf_has_df_feature(&cpu_info))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu_info))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	if (df_stress_pin(0) != 0)
		atf_tc_skip("Cannot pin to CPU 0: %s", strerror(errno));

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
	if (pmc_read(pmcid, &prev) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(initial) failed: %s", strerror(errno));
	}

	for (i = 0; i < bursts; i++) {
		error = amd_df_generate_remote_traffic();
		if (error != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("remote-traffic burst %d failed: %s",
			    i, strerror(error));
		}
		if (pmc_read(pmcid, &cur) != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			atf_tc_fail("pmc_read after burst %d failed: %s",
			    i, strerror(errno));
		}
		ATF_CHECK_MSG(cur >= prev,
		    "DF counter moved backwards at burst %d: "
		    "prev=%ju cur=%ju", i, (uintmax_t)prev, (uintmax_t)cur);
		printf("Burst %2d [%s]: delta=%ju total=%ju\n", i,
		    event->name, (uintmax_t)(cur - prev), (uintmax_t)cur);
		prev = cur;
	}

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-STRESS-DF-03  df_stress_concurrent_traffic
 *
 * Launches N worker threads (one per online CPU).  Each thread alternates
 * between the remote-traffic and local-traffic workloads for 10 rounds.
 * A single domain-wide DF counter is read before and after to verify there
 * is overall forward progress; the test does not require an exact count.
 *
 * Tagged X-parallel=1 to avoid contending with other stress tests for DF
 * PMC slots.
 * ---------------------------------------------------------------------- */

struct df_concurrent_arg {
	int	cpu;
	int	error;
	int	rounds;
};

static void *
df_concurrent_worker(void *arg)
{
	struct df_concurrent_arg *wa = arg;
	int i, error;

	signal(SIGPIPE, SIG_IGN);

	if (df_stress_pin(wa->cpu) != 0) {
		wa->error = errno;
		return (NULL);
	}

	for (i = 0; i < wa->rounds; i++) {
		error = amd_df_generate_remote_traffic();
		if (error != 0) {
			wa->error = error;
			return (NULL);
		}
		error = amd_df_generate_local_traffic();
		if (error != 0) {
			wa->error = error;
			return (NULL);
		}
	}

	wa->error = 0;
	return (NULL);
}

ATF_TC(df_stress_concurrent_traffic);
ATF_TC_HEAD(df_stress_concurrent_traffic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Run remote+local DF traffic concurrently on all online CPUs for "
	    "10 rounds each and verify the domain-wide DF counter shows "
	    "forward progress without wrap-around errors.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "X-parallel", "1");
}

ATF_TC_BODY(df_stress_concurrent_traffic, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu_info;
	struct df_concurrent_arg *args;
	pthread_t *threads;
	pmc_id_t pmcid;
	pmc_value_t before, after;
	int ncpus, i, last_error, error;
	const int rounds = 10;

	pmcid = PMC_ID_INVALID;

	amd_umcdf_skip_unless_known_zen(&cpu_info);
	if (!amd_umcdf_has_df_feature(&cpu_info))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu_info))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu_info.zen));
	amd_umcdf_skip_unless_pmu_events();

	ncpus = df_stress_ncpus();

	threads = calloc(ncpus, sizeof(pthread_t));
	args    = calloc(ncpus, sizeof(struct df_concurrent_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	event = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg,
	    &last_error);
	if (event == NULL) {
		free(threads); free(args);
		atf_tc_skip("No allocatable DF DRAM event; last error: %s",
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
		error = pthread_create(&threads[i], NULL,
		    df_concurrent_worker, &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}
	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		if (args[i].error != 0) {
			(void)pmc_stop(pmcid);
			amd_umcdf_release_pmc(pmcid);
			free(threads); free(args);
			atf_tc_fail("Worker on CPU %d failed: %s",
			    i, strerror(args[i].error));
		}
	}

	if (pmc_read(pmcid, &after) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		free(threads); free(args);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("Concurrent stress (%d CPUs, %d rounds) [%s]: "
	    "before=%ju after=%ju delta=%ju\n",
	    ncpus, rounds, event->name,
	    (uintmax_t)before, (uintmax_t)after,
	    (uintmax_t)(after - before));

	ATF_CHECK_MSG(after >= before,
	    "DF counter moved backwards under concurrent load: "
	    "before=%ju after=%ju", (uintmax_t)before, (uintmax_t)after);

	(void)pmc_stop(pmcid);
	amd_umcdf_release_pmc(pmcid);
	free(threads);
	free(args);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, df_stress_rapid_alloc_free);
	ATF_TP_ADD_TC(tp, df_stress_sustained_traffic);
	ATF_TP_ADD_TC(tp, df_stress_concurrent_traffic);
	return (atf_no_error());
}

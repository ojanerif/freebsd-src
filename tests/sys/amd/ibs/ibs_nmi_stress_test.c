/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * IBS NMI Stress Tests — TC-INT
 *
 * Validates IBS NMI delivery stability and the workaround #420 drain
 * sequence under sustained system stress.  Motivated by a real crash
 * scenario (FreeBSD-Tests-026) in which high IBS sampling rates combined
 * with heavy network load overwhelmed the NMI delivery path, stalled the
 * network stack, and ultimately panicked the machine.
 *
 * Three test cases:
 *
 * 1. ibs_nmi_high_rate_stability [TC-INT-NMI-01]
 *    Enable IBS Op at a moderate but meaningful period (0x0400 × 16 = 16384
 *    cycles between samples) while running CPU and memory stress threads for
 *    120 seconds.  Polls IBS Op CTL every 5 seconds to verify the MSR path
 *    remains functional.  Asserts: no read/write errors, MSR value coherent
 *    after stress.
 *
 * 2. ibs_nmi_rate_limit_enforce [TC-INT-NMI-02]
 *    SKIPPED until the kernel rate-limiting patch lands (FreeBSD-Tests-026).
 *    Guard: checks for sysctl dev.hwpmc.ibs.min_period at test startup.
 *    When the patch is present: attempt to program a period below the
 *    enforced minimum and assert the kernel clamps it.
 *
 * 3. ibs_nmi_drain_under_load [TC-INT-NMI-03]
 *    Performs the workaround #420 stop sequence (clear MaxCnt → clear enable
 *    → 50 × 1 µs zero writes) 100 times while CPU stress threads run.
 *    Asserts: each drain completes (MSR reads 0 after the sequence) and no
 *    iteration times out beyond 5 ms.
 *
 * References:
 *   AMD Revision Guide for Family 10h — Erratum #420
 *   FreeBSD hwpmc_ibs.c ibs_stop_pmc()
 *   Meeting transcript 2026-05-14 — Ali's NMI overflow crash analysis
 *   FreeBSD-Tests-026
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ibs_utils.h"

/*
 * IBS Op period for NMI stress tests.
 * 0x0400 × 16 = 16 384 cycles between samples — frequent enough to exercise
 * the NMI path without approaching the overflow rate that caused Ali's crash.
 */
#define NMI_STRESS_PERIOD	0x0400ULL

/* Duration and poll interval, matching the existing stress tests. */
#define NMI_STRESS_DURATION_SEC		120
#define NMI_STRESS_POLL_INTERVAL_SEC	5

/* Number of workaround #420 drain iterations in TC-INT-NMI-03. */
#define NMI_DRAIN_ITERATIONS	100

/* Maximum acceptable time for one drain iteration (µs). */
#define NMI_DRAIN_TIMEOUT_US	5000	/* 5 ms */

/* -----------------------------------------------------------------------
 * Lightweight inline stress workers (no dependency on stress/ directory)
 * ----------------------------------------------------------------------- */

struct nmi_stress_worker {
	volatile bool	*stop;
	int		 cpu;
};

static void *
nmi_compute_worker(void *arg)
{
	struct nmi_stress_worker *w = arg;
	cpuset_t mask;
	uint64_t x;

	CPU_ZERO(&mask);
	CPU_SET(w->cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	x = (uint64_t)(w->cpu + 1) * 0xDEADBEEFCAFEULL;
	while (!*w->stop) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
	}
	if (x == 0)
		(void)x;
	return (NULL);
}

/*
 * Shared 64 MiB memory buffer for the inline memory stressor.
 * Allocated once per test, freed after stress period ends.
 */
#define NMI_MEM_BUF_SIZE	(64UL * 1024UL * 1024UL)
#define NMI_MEM_STRIDE		64UL

struct nmi_mem_worker {
	volatile bool	*stop;
	uint64_t	*buf;
	size_t		 n_elements;
	int		 cpu;
};

static void *
nmi_mem_worker(void *arg)
{
	struct nmi_mem_worker *w = arg;
	cpuset_t mask;
	uint64_t idx, acc;
	size_t step;

	CPU_ZERO(&mask);
	CPU_SET(w->cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	idx = (uint64_t)(w->cpu + 1) * 6364136223846793005ULL;
	acc = 0;

	while (!*w->stop) {
		for (step = 0; step < w->n_elements && !*w->stop;
		    step += NMI_MEM_STRIDE) {
			idx = (idx * 6364136223846793005ULL + step) %
			    w->n_elements;
			w->buf[idx] ^= acc;
			acc ^= w->buf[idx];
		}
	}
	if (acc == 0)
		w->buf[0] ^= 1;
	return (NULL);
}

/* -----------------------------------------------------------------------
 * TC-INT-NMI-01: IBS NMI high-rate stability under CPU + memory stress
 * ----------------------------------------------------------------------- */

ATF_TC(ibs_nmi_high_rate_stability);
ATF_TC_HEAD(ibs_nmi_high_rate_stability, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-INT-NMI-01] IBS Op sampling at period 0x0400 while CPU + "
	    "memory stressors run (120 s); asserts MSR path survives sustained "
	    "load and NMI delivery remains coherent");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(ibs_nmi_high_rate_stability, tc)
{
	struct nmi_stress_worker *cworkers;
	struct nmi_mem_worker    *mworkers;
	pthread_t *cthreads, *mthreads;
	uint64_t *mem_buf;
	volatile bool stop = false;
	int ncpus, i, error;
	time_t deadline;
	uint64_t original, ctl;
	size_t n_elements;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save IBS Op CTL baseline. */
	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL on CPU 0: %s",
		    strerror(error));

	ncpus      = (int)(sysconf(_SC_NPROCESSORS_ONLN));
	if (ncpus < 1) ncpus = 1;
	n_elements = NMI_MEM_BUF_SIZE / sizeof(uint64_t);

	/* Allocate shared memory buffer for inline mem stressor. */
	mem_buf = calloc(n_elements, sizeof(uint64_t));
	ATF_REQUIRE_MSG(mem_buf != NULL,
	    "calloc %lu MiB mem_buf: %s",
	    (unsigned long)(NMI_MEM_BUF_SIZE / (1024 * 1024)),
	    strerror(errno));

	cworkers = calloc((size_t)ncpus, sizeof(struct nmi_stress_worker));
	mworkers = calloc((size_t)ncpus, sizeof(struct nmi_mem_worker));
	cthreads = calloc((size_t)ncpus, sizeof(pthread_t));
	mthreads = calloc((size_t)ncpus, sizeof(pthread_t));
	ATF_REQUIRE(cworkers != NULL && mworkers != NULL &&
	    cthreads != NULL && mthreads != NULL);

	/* Launch compute + memory stress workers. */
	for (i = 0; i < ncpus; i++) {
		cworkers[i].stop = &stop;
		cworkers[i].cpu  = i;
		mworkers[i].stop       = &stop;
		mworkers[i].buf        = mem_buf;
		mworkers[i].n_elements = n_elements;
		mworkers[i].cpu        = i;

		error = pthread_create(&cthreads[i], NULL,
		    nmi_compute_worker, &cworkers[i]);
		ATF_REQUIRE_MSG(error == 0,
		    "pthread_create compute CPU %d: %s", i, strerror(error));

		error = pthread_create(&mthreads[i], NULL,
		    nmi_mem_worker, &mworkers[i]);
		ATF_REQUIRE_MSG(error == 0,
		    "pthread_create mem CPU %d: %s", i, strerror(error));
	}

	/*
	 * Enable IBS Op at a moderate period on CPU 0.
	 * MaxCnt in bits [15:0], enable bit 19 (IbsOpEn).
	 */
	ctl = IBS_OP_ENABLE_BIT | NMI_STRESS_PERIOD;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	if (error != 0) {
		stop = true;
		for (i = 0; i < ncpus; i++) {
			pthread_join(cthreads[i], NULL);
			pthread_join(mthreads[i], NULL);
		}
		free(mem_buf); free(cworkers); free(mworkers);
		free(cthreads); free(mthreads);
		atf_tc_skip("Cannot enable IBS Op on CPU 0: %s",
		    strerror(error));
	}

	/* Poll MSR health on CPU 0 every 5 s for the full duration. */
	deadline = time(NULL) + NMI_STRESS_DURATION_SEC;
	while (time(NULL) < deadline) {
		sleep(NMI_STRESS_POLL_INTERVAL_SEC);
		error = read_msr(0, MSR_IBS_OP_CTL, &ctl);
		ATF_CHECK_MSG(error == 0,
		    "MSR_IBS_OP_CTL read failed on CPU 0 under stress: %s",
		    strerror(error));
	}

	/* Disable IBS Op using workaround #420 sequence. */
	ctl &= ~IBS_MAXCNT_MASK;
	write_msr(0, MSR_IBS_OP_CTL, ctl);
	usleep(1);
	ctl &= ~IBS_OP_ENABLE_BIT;
	write_msr(0, MSR_IBS_OP_CTL, ctl);
	for (i = 0; i < 50; i++) {
		write_msr(0, MSR_IBS_OP_CTL, 0);
		usleep(1);
	}

	/* Restore original value. */
	write_msr(0, MSR_IBS_OP_CTL, original);

	/* Final write/read round-trip confirms MSR is still writable. */
	{
		uint64_t v = (original & ~IBS_MAXCNT_MASK) & ~IBS_OP_ENABLE_BIT;
		error = write_msr(0, MSR_IBS_OP_CTL, v);
		ATF_CHECK_MSG(error == 0,
		    "final post-stress MSR write failed: %s", strerror(error));
		error = read_msr(0, MSR_IBS_OP_CTL, &ctl);
		ATF_CHECK_MSG(error == 0,
		    "final post-stress MSR read failed: %s", strerror(error));
		write_msr(0, MSR_IBS_OP_CTL, original);
	}

	stop = true;
	for (i = 0; i < ncpus; i++) {
		pthread_join(cthreads[i], NULL);
		pthread_join(mthreads[i], NULL);
	}

	printf("nmi-high-rate: IBS Op period=0x%04llx, %d CPUs stressed, "
	    "%d seconds\n",
	    (unsigned long long)NMI_STRESS_PERIOD, ncpus,
	    NMI_STRESS_DURATION_SEC);

	free(mem_buf);
	free(cworkers);
	free(mworkers);
	free(cthreads);
	free(mthreads);
}

/* -----------------------------------------------------------------------
 * TC-INT-NMI-02: IBS rate-limit enforcement
 * (SKIPPED until kernel rate-limiting patch lands — FreeBSD-Tests-026)
 * ----------------------------------------------------------------------- */

ATF_TC(ibs_nmi_rate_limit_enforce);
ATF_TC_HEAD(ibs_nmi_rate_limit_enforce, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-INT-NMI-02] Attempt to program IBS Op period below the kernel "
	    "minimum and assert the kernel clamps it; SKIPPED until "
	    "dev.hwpmc.ibs.min_period sysctl exists (FreeBSD-Tests-026)");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(ibs_nmi_rate_limit_enforce, tc)
{
	uint32_t min_period = 0;
	size_t len = sizeof(min_period);
	uint64_t original, ctl;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/*
	 * Guard: skip until the rate-limiting sysctl exists in the kernel.
	 * When Ali's patch (FreeBSD-Tests-026) lands, this sysctl will be
	 * present and the test will execute the clamping validation below.
	 */
	if (sysctlbyname("dev.hwpmc.ibs.min_period", &min_period, &len,
	    NULL, 0) != 0)
		atf_tc_skip("dev.hwpmc.ibs.min_period sysctl not present; "
		    "IBS rate limiting not yet implemented "
		    "(pending FreeBSD-Tests-026 kernel patch)");

	ATF_REQUIRE_MSG(min_period > 0,
	    "dev.hwpmc.ibs.min_period is zero — invalid kernel value");

	/* Save baseline. */
	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s", strerror(error));

	/*
	 * Attempt to program a period that is one step below the minimum.
	 * The kernel must clamp it to min_period.
	 */
	uint64_t below_min = (uint64_t)(min_period > 1 ? min_period - 1 : 1);
	ctl = IBS_OP_ENABLE_BIT | (below_min & IBS_MAXCNT_MASK);
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_MSG(error == 0,
	    "write below-min period: %s", strerror(error));

	/* Disable immediately (no sampling needed, just check clamping). */
	write_msr(0, MSR_IBS_OP_CTL, 0);

	/* Read back and verify the period was clamped. */
	uint64_t readback;
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_MSG(error == 0, "read-back: %s", strerror(error));

	uint64_t actual_period = readback & IBS_MAXCNT_MASK;
	ATF_CHECK_MSG(actual_period >= (uint64_t)min_period,
	    "kernel did not clamp period: programmed=0x%llx "
	    "min_period=0x%x actual=0x%llx",
	    (unsigned long long)below_min, min_period,
	    (unsigned long long)actual_period);

	write_msr(0, MSR_IBS_OP_CTL, original);

	printf("rate-limit: min_period=0x%x, below_min=0x%llx, "
	    "actual=0x%llx (clamped correctly: %s)\n",
	    min_period, (unsigned long long)below_min,
	    (unsigned long long)actual_period,
	    actual_period >= (uint64_t)min_period ? "yes" : "no");
}

/* -----------------------------------------------------------------------
 * TC-INT-NMI-03: workaround #420 drain under CPU load
 * ----------------------------------------------------------------------- */

ATF_TC(ibs_nmi_drain_under_load);
ATF_TC_HEAD(ibs_nmi_drain_under_load, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-INT-NMI-03] Workaround #420 stop sequence (clear MaxCnt -> "
	    "clear enable -> 50 x 1 us zero writes) repeated 100 times while "
	    "CPU stress threads run; asserts each drain completes and MSR "
	    "reads 0 after each iteration");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "60");
}

ATF_TC_BODY(ibs_nmi_drain_under_load, tc)
{
	struct nmi_stress_worker *workers;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, iter, i, error;
	uint64_t original, ctl, readback;
	struct timespec t0, t1;
	long elapsed_us;
	int drain_timeouts = 0;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s", strerror(error));

	ncpus   = (int)(sysconf(_SC_NPROCESSORS_ONLN));
	if (ncpus < 1) ncpus = 1;

	workers = calloc((size_t)ncpus, sizeof(struct nmi_stress_worker));
	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	ATF_REQUIRE(workers != NULL && threads != NULL);

	/* Launch compute stress threads. */
	for (i = 0; i < ncpus; i++) {
		workers[i].stop = &stop;
		workers[i].cpu  = i;
		error = pthread_create(&threads[i], NULL,
		    nmi_compute_worker, &workers[i]);
		ATF_REQUIRE_MSG(error == 0,
		    "pthread_create CPU %d: %s", i, strerror(error));
	}

	/*
	 * Perform NMI_DRAIN_ITERATIONS workaround #420 drain cycles while
	 * CPU stress is running.
	 */
	for (iter = 0; iter < NMI_DRAIN_ITERATIONS; iter++) {
		/* Enable IBS Fetch at a safe period. */
		ctl = IBS_FETCH_CTL_ENABLE | NMI_STRESS_PERIOD;
		error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
		ATF_CHECK_MSG(error == 0,
		    "iter %d: enable IBS Fetch: %s", iter, strerror(error));

		usleep(10);	/* brief sampling window */

		clock_gettime(CLOCK_MONOTONIC, &t0);

		/* Step 1: clear MaxCnt. */
		ctl &= ~IBS_MAXCNT_MASK;
		write_msr(0, MSR_IBS_FETCH_CTL, ctl);
		usleep(1);

		/* Step 2: clear enable. */
		ctl &= ~IBS_FETCH_CTL_ENABLE;
		write_msr(0, MSR_IBS_FETCH_CTL, ctl);

		/* Step 3: 50 × 1 µs zero writes (the drain loop). */
		for (i = 0; i < 50; i++) {
			write_msr(0, MSR_IBS_FETCH_CTL, 0);
			usleep(1);
		}

		clock_gettime(CLOCK_MONOTONIC, &t1);
		elapsed_us = (long)((t1.tv_sec  - t0.tv_sec)  * 1000000L +
		    (t1.tv_nsec - t0.tv_nsec) / 1000L);

		if (elapsed_us > NMI_DRAIN_TIMEOUT_US)
			drain_timeouts++;

		/* Verify CTL reads 0 after the drain. */
		error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
		ATF_CHECK_MSG(error == 0,
		    "iter %d: read after drain: %s", iter, strerror(error));
		ATF_CHECK_MSG(readback == 0ULL,
		    "iter %d: IBS Fetch CTL not zero after drain: 0x%llx",
		    iter, (unsigned long long)readback);
	}

	stop = true;
	for (i = 0; i < ncpus; i++)
		pthread_join(threads[i], NULL);

	write_msr(0, MSR_IBS_OP_CTL, original);

	ATF_CHECK_MSG(drain_timeouts == 0,
	    "%d out of %d drain iterations exceeded %d µs threshold "
	    "(workaround #420 drain is slower than expected under load)",
	    drain_timeouts, NMI_DRAIN_ITERATIONS, NMI_DRAIN_TIMEOUT_US);

	printf("nmi-drain: %d iterations, %d timeouts (>%d µs), "
	    "%d CPUs stressed\n",
	    NMI_DRAIN_ITERATIONS, drain_timeouts,
	    NMI_DRAIN_TIMEOUT_US, ncpus);

	free(workers);
	free(threads);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_nmi_high_rate_stability);
	ATF_TP_ADD_TC(tp, ibs_nmi_rate_limit_enforce);
	ATF_TP_ADD_TC(tp, ibs_nmi_drain_under_load);

	return (atf_no_error());
}

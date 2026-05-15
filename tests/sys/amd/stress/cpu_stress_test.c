/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * CPU Stress Tests — TC-CSTR
 *
 * Standalone CPU subsystem stress validation.  Three workloads:
 *
 * 1. cpu_stress_compute [TC-CSTR-01]
 *    One thread per CPU pinned to its logical core, running a tight
 *    xorshift64 PRNG loop for 120 seconds.  Saturates integer ALU and
 *    exercises the retirement pipeline.  Asserts all threads complete
 *    and each produces a non-zero iteration count.
 *
 * 2. cpu_stress_context_switch [TC-CSTR-02]
 *    4 × ncpus threads calling sched_yield() in a tight loop for 120
 *    seconds.  Oversubscribes the CPU set to maximise scheduler churn.
 *    Asserts total yield count is positive (scheduler not stalled).
 *
 * 3. cpu_stress_fpu [TC-CSTR-03]
 *    One thread per CPU running a sin(x)*cos(x) accumulator loop for
 *    120 seconds.  Exercises FPU save/restore on context switches.
 *    Asserts the accumulated result is finite (NaN or Inf would
 *    indicate FPU state corruption).
 *
 * These tests produce background load for use with --with-stress.
 * They do not require root or any hardware access.
 */

#include <atf-c.h>
#include <math.h>
#include <stdio.h>

#include "stress_utils.h"

/* -----------------------------------------------------------------------
 * TC-CSTR-01: integer compute (xorshift64 ALU saturation)
 * ----------------------------------------------------------------------- */

struct compute_arg {
	volatile bool	*stop;
	int		 cpu;
	int		 error;
	uint64_t	 iterations;
};

static void *
compute_thread(void *arg)
{
	struct compute_arg *ca = arg;
	cpuset_t mask;
	uint64_t x;

	CPU_ZERO(&mask);
	CPU_SET(ca->cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	x = (uint64_t)(ca->cpu + 1) * 0xBEEFCAFEDEAD0001ULL;

	while (!*ca->stop) {
		STRESS_XORSHIFT64(x);
		ca->iterations++;
	}

	if (x == 0)
		ca->iterations++;	/* prevent dead-code elimination */

	ca->error = 0;
	return (NULL);
}

ATF_TC(cpu_stress_compute);
ATF_TC_HEAD(cpu_stress_compute, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-CSTR-01] Integer ALU saturation via per-CPU xorshift64 "
	    "loop (120 s)");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(cpu_stress_compute, tc)
{
	struct compute_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, i, error;
	time_t deadline;
	uint64_t total = 0;

	ncpus   = stress_ncpus();
	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(struct compute_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	for (i = 0; i < ncpus; i++) {
		args[i].stop       = &stop;
		args[i].cpu        = i;
		args[i].error      = -1;
		args[i].iterations = 0;

		error = pthread_create(&threads[i], NULL, compute_thread,
		    &args[i]);
		if (error != 0) {
			stop = true;
			free(threads);
			free(args);
			atf_tc_fail("pthread_create CPU %d: %s", i,
			    strerror(error));
		}
	}

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;

	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		ATF_CHECK_MSG(args[i].error == 0,
		    "compute thread CPU %d failed: %s", i,
		    strerror(args[i].error));
		ATF_CHECK_MSG(args[i].iterations > 0,
		    "compute thread CPU %d produced zero iterations", i);
		total += args[i].iterations;
	}

	printf("compute: %llu total xorshift64 iterations across %d CPUs "
	    "in %d seconds\n",
	    (unsigned long long)total, ncpus, STRESS_DURATION_SEC);

	free(threads);
	free(args);
}

/* -----------------------------------------------------------------------
 * TC-CSTR-02: context-switch storm (scheduler pressure)
 * ----------------------------------------------------------------------- */

struct ctx_switch_arg {
	volatile bool	*stop;
	int		 error;
	uint64_t	 yields;
};

static void *
ctx_switch_thread(void *arg)
{
	struct ctx_switch_arg *csa = arg;

	while (!*csa->stop) {
		sched_yield();
		csa->yields++;
	}

	csa->error = 0;
	return (NULL);
}

ATF_TC(cpu_stress_context_switch);
ATF_TC_HEAD(cpu_stress_context_switch, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-CSTR-02] Scheduler pressure via 4 x ncpus sched_yield() "
	    "threads (120 s)");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(cpu_stress_context_switch, tc)
{
	struct ctx_switch_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, nthreads, i, error;
	time_t deadline;
	uint64_t total = 0;

	ncpus    = stress_ncpus();
	nthreads = ncpus * 4;

	threads = calloc((size_t)nthreads, sizeof(pthread_t));
	args    = calloc((size_t)nthreads, sizeof(struct ctx_switch_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	for (i = 0; i < nthreads; i++) {
		args[i].stop   = &stop;
		args[i].error  = -1;
		args[i].yields = 0;

		error = pthread_create(&threads[i], NULL, ctx_switch_thread,
		    &args[i]);
		if (error != 0) {
			stop = true;
			free(threads);
			free(args);
			atf_tc_fail("pthread_create thread %d: %s", i,
			    strerror(error));
		}
	}

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;

	for (i = 0; i < nthreads; i++) {
		pthread_join(threads[i], NULL);
		ATF_CHECK_MSG(args[i].error == 0,
		    "ctx-switch thread %d failed: %s", i,
		    strerror(args[i].error));
		total += args[i].yields;
	}

	ATF_CHECK_MSG(total > 0,
	    "zero total sched_yield calls — scheduler may be stalled");

	printf("ctx-switch: %llu total sched_yield calls across %d threads "
	    "(%d CPUs) in %d seconds\n",
	    (unsigned long long)total, nthreads, ncpus, STRESS_DURATION_SEC);

	free(threads);
	free(args);
}

/* -----------------------------------------------------------------------
 * TC-CSTR-03: FPU accumulator (FPU save/restore path)
 * ----------------------------------------------------------------------- */

struct fpu_arg {
	volatile bool	*stop;
	int		 cpu;
	int		 error;
	double		 result;
	uint64_t	 iterations;
};

static void *
fpu_thread(void *arg)
{
	struct fpu_arg *fa = arg;
	cpuset_t mask;
	double x, acc;

	CPU_ZERO(&mask);
	CPU_SET(fa->cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	x   = (double)(fa->cpu + 1) * 0.123456789;
	acc = 0.0;

	while (!*fa->stop) {
		acc += sin(x) * cos(x);
		x   += 1e-7;
		fa->iterations++;
	}

	fa->result = acc;
	fa->error  = 0;
	return (NULL);
}

ATF_TC(cpu_stress_fpu);
ATF_TC_HEAD(cpu_stress_fpu, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-CSTR-03] FPU state integrity — per-CPU sin/cos accumulator "
	    "loop (120 s); NaN or Inf indicates FPU corruption");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(cpu_stress_fpu, tc)
{
	struct fpu_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, i, error;
	time_t deadline;

	ncpus   = stress_ncpus();
	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(struct fpu_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	for (i = 0; i < ncpus; i++) {
		args[i].stop       = &stop;
		args[i].cpu        = i;
		args[i].error      = -1;
		args[i].result     = 0.0;
		args[i].iterations = 0;

		error = pthread_create(&threads[i], NULL, fpu_thread,
		    &args[i]);
		if (error != 0) {
			stop = true;
			free(threads);
			free(args);
			atf_tc_fail("pthread_create CPU %d: %s", i,
			    strerror(error));
		}
	}

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;

	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		ATF_CHECK_MSG(args[i].error == 0,
		    "FPU thread CPU %d failed: %s", i, strerror(args[i].error));
		ATF_CHECK_MSG(isfinite(args[i].result),
		    "FPU thread CPU %d returned non-finite result %g "
		    "(NaN or Inf indicates FPU state corruption)", i,
		    args[i].result);
	}

	printf("fpu: %d CPUs ran sin/cos loop for %d seconds\n",
	    ncpus, STRESS_DURATION_SEC);

	free(threads);
	free(args);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, cpu_stress_compute);
	ATF_TP_ADD_TC(tp, cpu_stress_context_switch);
	ATF_TP_ADD_TC(tp, cpu_stress_fpu);

	return (atf_no_error());
}

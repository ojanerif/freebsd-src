/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * cpu_stressor — background CPU load generator
 *
 * Spawns one xorshift64 compute thread + one sched_yield() thread per CPU
 * and runs until SIGTERM or SIGINT.  Used by run.sh --with-stress to
 * simulate CPU pressure while another test suite executes.
 *
 * Exit code: 0 on clean stop, 1 on internal error.
 */

#include <signal.h>
#include <stdio.h>

#include "stress_utils.h"

static volatile sig_atomic_t g_stop = 0;

static void
handle_stop(int sig __unused)
{
	g_stop = 1;
}

/* Compute worker */
static void *
compute_worker(void *arg)
{
	int cpu = *(int *)arg;
	cpuset_t mask;
	uint64_t x;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	x = (uint64_t)(cpu + 1) * 0xBEEFCAFEDEAD0001ULL;
	while (!g_stop) {
		STRESS_XORSHIFT64(x);
	}
	if (x == 0)
		(void)x;
	return (NULL);
}

/* Yield worker */
static void *
yield_worker(void *arg __unused)
{
	while (!g_stop)
		sched_yield();
	return (NULL);
}

int
main(void)
{
	pthread_t *threads;
	int *cpu_ids;
	int ncpus, nthreads, i;

	signal(SIGTERM, handle_stop);
	signal(SIGINT,  handle_stop);

	ncpus    = stress_ncpus();
	nthreads = ncpus * 2;	/* one compute + one yield per CPU */

	threads  = calloc((size_t)nthreads, sizeof(pthread_t));
	cpu_ids  = calloc((size_t)ncpus,    sizeof(int));
	if (threads == NULL || cpu_ids == NULL) {
		fprintf(stderr, "cpu_stressor: calloc: %s\n", strerror(errno));
		return (1);
	}

	for (i = 0; i < ncpus; i++)
		cpu_ids[i] = i;

	for (i = 0; i < ncpus; i++) {
		if (pthread_create(&threads[i], NULL, compute_worker,
		    &cpu_ids[i]) != 0) {
			g_stop = 1;
			return (1);
		}
		if (pthread_create(&threads[ncpus + i], NULL, yield_worker,
		    NULL) != 0) {
			g_stop = 1;
			return (1);
		}
	}

	/* Wait until signalled. */
	while (!g_stop)
		sleep(1);

	for (i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	fprintf(stderr, "cpu_stressor[%d]: stopped cleanly (ncpus=%d)\n",
	    (int)getpid(), ncpus);

	free(threads);
	free(cpu_ids);
	return (0);
}

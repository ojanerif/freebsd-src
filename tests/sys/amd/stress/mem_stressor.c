/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * mem_stressor — background memory load generator
 *
 * Runs cache-thrash (128 MiB shared buffer) and bandwidth sweep
 * (16 MiB private per thread) workloads until SIGTERM or SIGINT.
 * Used by run.sh --with-stress to simulate memory pressure while
 * another test suite executes.
 *
 * Exit code: 0 on clean stop, 1 on internal error.
 */

#include <signal.h>
#include <stdio.h>

#include "stress_utils.h"

#define MEM_STR_THRASH_SIZE	(128UL * 1024UL * 1024UL)
#define MEM_STR_BW_SIZE		(16UL  * 1024UL * 1024UL)
#define MEM_STR_STRIDE		64UL

static volatile sig_atomic_t g_stop = 0;

static void
handle_stop(int sig __unused)
{
	g_stop = 1;
}

/* Shared thrash buffer and size (set before threads start). */
static uint64_t *g_thrash_buf;
static size_t    g_thrash_nelems;

static void *
thrash_worker(void *arg)
{
	int cpu = *(int *)arg;
	cpuset_t mask;
	uint64_t idx, acc;
	size_t step;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	idx = (uint64_t)(cpu + 1) * STRESS_LCG_MUL;
	acc = 0;

	while (!g_stop) {
		for (step = 0; step < g_thrash_nelems && !g_stop;
		    step += MEM_STR_STRIDE) {
			idx = (idx * STRESS_LCG_MUL + step) % g_thrash_nelems;
			g_thrash_buf[idx] ^= acc;
			acc ^= g_thrash_buf[idx];
		}
	}
	if (acc == 0)
		g_thrash_buf[0] ^= 1;
	return (NULL);
}

static void *
bw_worker(void *arg)
{
	int cpu = *(int *)arg;
	cpuset_t mask;
	uint64_t *buf;
	size_t n, j;
	uint64_t sum, sweeps = 0;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	n   = MEM_STR_BW_SIZE / sizeof(uint64_t);
	buf = calloc(n, sizeof(uint64_t));
	if (buf == NULL)
		return (NULL);

	while (!g_stop) {
		for (j = 0; j < n && !g_stop; j++)
			buf[j] = (uint64_t)j ^ sweeps;
		sum = 0;
		for (j = 0; j < n && !g_stop; j++)
			sum += buf[j];
		if (sum == 0)
			buf[0] = 1;
		sweeps++;
	}

	free(buf);
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

	ncpus   = stress_ncpus();
	nthreads = ncpus * 2;	/* one thrash + one bw per CPU */

	g_thrash_nelems = MEM_STR_THRASH_SIZE / sizeof(uint64_t);
	g_thrash_buf = calloc(g_thrash_nelems, sizeof(uint64_t));
	if (g_thrash_buf == NULL) {
		fprintf(stderr, "mem_stressor: calloc thrash buf: %s\n",
		    strerror(errno));
		return (1);
	}

	threads = calloc((size_t)nthreads, sizeof(pthread_t));
	cpu_ids = calloc((size_t)ncpus,    sizeof(int));
	if (threads == NULL || cpu_ids == NULL) {
		free(g_thrash_buf);
		fprintf(stderr, "mem_stressor: calloc: %s\n", strerror(errno));
		return (1);
	}

	for (i = 0; i < ncpus; i++)
		cpu_ids[i] = i;

	for (i = 0; i < ncpus; i++) {
		if (pthread_create(&threads[i], NULL, thrash_worker,
		    &cpu_ids[i]) != 0) {
			g_stop = 1;
			return (1);
		}
		if (pthread_create(&threads[ncpus + i], NULL, bw_worker,
		    &cpu_ids[i]) != 0) {
			g_stop = 1;
			return (1);
		}
	}

	while (!g_stop)
		sleep(1);

	for (i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	fprintf(stderr, "mem_stressor[%d]: stopped cleanly (ncpus=%d)\n",
	    (int)getpid(), ncpus);

	free(g_thrash_buf);
	free(threads);
	free(cpu_ids);
	return (0);
}

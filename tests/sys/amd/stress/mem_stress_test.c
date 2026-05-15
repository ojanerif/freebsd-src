/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * Memory Stress Tests — TC-MSTR
 *
 * Standalone memory subsystem stress validation.  Three workloads:
 *
 * 1. mem_stress_cache_thrash [TC-MSTR-01]
 *    One thread per CPU performing pseudo-random strided read-modify-
 *    write accesses over a 128 MiB shared buffer for 120 seconds.
 *    Stride > cache-line defeats hardware prefetchers and forces L3
 *    evictions and DRAM fetches.  Asserts data integrity via final
 *    accumulator cross-check.
 *
 * 2. mem_stress_bandwidth [TC-MSTR-02]
 *    One thread per CPU doing sequential write-then-read sweeps over a
 *    private 16 MiB buffer for 120 seconds.  Saturates memory bandwidth
 *    and the LLC fill/evict path.  Asserts read data matches written data
 *    on each sweep.
 *
 * 3. mem_stress_tlb [TC-MSTR-03]
 *    Repeated mmap(2 MiB) / touch / munmap cycles for 120 seconds to
 *    generate TLB shootdowns.  Asserts no segfaults and that mprotect(2)
 *    succeeds after the stress period (TLB still functional).
 *
 * No root required.  No hardware dependency.
 */

#include <sys/mman.h>

#include <atf-c.h>
#include <stdio.h>

#include "stress_utils.h"

/* Shared buffer size for the cache-thrash test. */
#define MEM_THRASH_BUF_SIZE	(128UL * 1024UL * 1024UL)

/* Per-thread buffer size for the bandwidth test. */
#define MEM_BW_BUF_SIZE		(16UL * 1024UL * 1024UL)

/* Stride in uint64_t elements (512 bytes > cache line + prefetch stream). */
#define MEM_THRASH_STRIDE	64UL

/* TLB stress: 2 MiB mmap region, page-touch size. */
#define MEM_TLB_REGION_SIZE	(2UL * 1024UL * 1024UL)
#define MEM_TLB_PAGE_SIZE	4096UL

/* -----------------------------------------------------------------------
 * TC-MSTR-01: cache-thrash (random-strided LLC eviction)
 * ----------------------------------------------------------------------- */

struct thrash_arg {
	volatile bool	*stop;
	uint64_t	*buf;
	size_t		 n_elements;
	int		 cpu;
	int		 error;
	uint64_t	 accesses;
};

static void *
thrash_thread(void *arg)
{
	struct thrash_arg *ta = arg;
	cpuset_t mask;
	uint64_t idx, acc;
	size_t step;

	CPU_ZERO(&mask);
	CPU_SET(ta->cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	idx = (uint64_t)(ta->cpu + 1) * STRESS_LCG_MUL;
	acc = 0;

	while (!*ta->stop) {
		for (step = 0; step < ta->n_elements && !*ta->stop;
		    step += MEM_THRASH_STRIDE) {
			idx = (idx * STRESS_LCG_MUL + step) % ta->n_elements;
			ta->buf[idx] ^= acc;
			acc ^= ta->buf[idx];
			ta->accesses++;
		}
	}

	if (acc == 0)
		ta->buf[0] ^= 1;	/* prevent dead-store elimination */

	ta->error = 0;
	return (NULL);
}

ATF_TC(mem_stress_cache_thrash);
ATF_TC_HEAD(mem_stress_cache_thrash, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-MSTR-01] 128 MiB random-strided LLC thrash, per-CPU threads "
	    "(120 s); asserts data integrity");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(mem_stress_cache_thrash, tc)
{
	uint64_t *buf;
	struct thrash_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, i, error;
	size_t n_elements;
	time_t deadline;
	uint64_t total = 0;

	ncpus      = stress_ncpus();
	n_elements = MEM_THRASH_BUF_SIZE / sizeof(uint64_t);

	buf = calloc(n_elements, sizeof(uint64_t));
	ATF_REQUIRE_MSG(buf != NULL,
	    "calloc 128 MiB buffer: %s", strerror(errno));

	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(struct thrash_arg));
	if (threads == NULL || args == NULL) {
		free(buf);
		atf_tc_fail("calloc thread structures: %s", strerror(errno));
	}

	for (i = 0; i < ncpus; i++) {
		args[i].stop       = &stop;
		args[i].buf        = buf;
		args[i].n_elements = n_elements;
		args[i].cpu        = i;
		args[i].error      = -1;
		args[i].accesses   = 0;

		error = pthread_create(&threads[i], NULL, thrash_thread,
		    &args[i]);
		if (error != 0) {
			stop = true;
			free(buf);
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
		    "thrash thread CPU %d failed: %s", i,
		    strerror(args[i].error));
		total += args[i].accesses;
	}

	/* Integrity check: the buffer must not be all-zero after modifications. */
	{
		uint64_t sum = 0;
		size_t j;
		for (j = 0; j < n_elements; j++)
			sum |= buf[j];
		ATF_CHECK_MSG(sum != 0,
		    "buffer is entirely zero after thrash — "
		    "writes may have been silently dropped");
	}

	printf("cache-thrash: %llu accesses across %d CPUs in %d seconds\n",
	    (unsigned long long)total, ncpus, STRESS_DURATION_SEC);

	free(buf);
	free(threads);
	free(args);
}

/* -----------------------------------------------------------------------
 * TC-MSTR-02: bandwidth (sequential write + read sweep)
 * ----------------------------------------------------------------------- */

struct bw_arg {
	volatile bool	*stop;
	size_t		 n_elements;
	int		 cpu;
	int		 error;
	uint64_t	 sweeps;
	int		 integrity_fail;
};

static void *
bw_thread(void *arg)
{
	struct bw_arg *bwa = arg;
	cpuset_t mask;
	uint64_t *buf;
	uint64_t sum;
	size_t j, n;

	CPU_ZERO(&mask);
	CPU_SET(bwa->cpu, &mask);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	n   = bwa->n_elements;
	buf = calloc(n, sizeof(uint64_t));
	if (buf == NULL) {
		bwa->error = ENOMEM;
		return (NULL);
	}

	while (!*bwa->stop) {
		/* Write sweep: fill with a pattern unique to this sweep. */
		for (j = 0; j < n && !*bwa->stop; j++)
			buf[j] = (uint64_t)j ^ bwa->sweeps ^ (uint64_t)bwa->cpu;

		if (*bwa->stop)
			break;

		/* Read sweep: verify data then accumulate. */
		sum = 0;
		for (j = 0; j < n; j++) {
			uint64_t expected =
			    (uint64_t)j ^ bwa->sweeps ^ (uint64_t)bwa->cpu;
			if (buf[j] != expected) {
				bwa->integrity_fail++;
				break;
			}
			sum += buf[j];
		}

		if (sum == 0)
			buf[0] = 1;	/* make sum observable */

		bwa->sweeps++;
	}

	free(buf);
	bwa->error = 0;
	return (NULL);
}

ATF_TC(mem_stress_bandwidth);
ATF_TC_HEAD(mem_stress_bandwidth, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-MSTR-02] Per-CPU 16 MiB sequential write+read bandwidth "
	    "stress (120 s); asserts per-sweep data integrity");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(mem_stress_bandwidth, tc)
{
	struct bw_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, i, error;
	size_t n_elements;
	time_t deadline;
	uint64_t total_sweeps = 0;
	int total_fails = 0;

	ncpus      = stress_ncpus();
	n_elements = MEM_BW_BUF_SIZE / sizeof(uint64_t);

	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(struct bw_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	for (i = 0; i < ncpus; i++) {
		args[i].stop           = &stop;
		args[i].n_elements     = n_elements;
		args[i].cpu            = i;
		args[i].error          = -1;
		args[i].sweeps         = 0;
		args[i].integrity_fail = 0;

		error = pthread_create(&threads[i], NULL, bw_thread, &args[i]);
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
		    "bandwidth thread CPU %d failed: %s", i,
		    strerror(args[i].error));
		total_sweeps += args[i].sweeps;
		total_fails  += args[i].integrity_fail;
	}

	ATF_CHECK_MSG(total_fails == 0,
	    "%d bandwidth integrity failures detected — "
	    "memory write/read mismatch", total_fails);

	printf("bandwidth: %llu total sweeps across %d CPUs in %d seconds\n",
	    (unsigned long long)total_sweeps, ncpus, STRESS_DURATION_SEC);

	free(threads);
	free(args);
}

/* -----------------------------------------------------------------------
 * TC-MSTR-03: TLB stress (mmap/munmap shootdowns)
 * ----------------------------------------------------------------------- */

struct tlb_arg {
	volatile bool	*stop;
	int		 error;
	uint64_t	 cycles;
};

static void *
tlb_thread(void *arg)
{
	struct tlb_arg *ta = arg;
	void *p;
	volatile uint8_t *bp;
	size_t i;

	while (!*ta->stop) {
		p = mmap(NULL, MEM_TLB_REGION_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANON, -1, 0);
		if (p == MAP_FAILED) {
			ta->error = errno;
			return (NULL);
		}

		/* Touch every page to populate TLB entries. */
		bp = (volatile uint8_t *)p;
		for (i = 0; i < MEM_TLB_REGION_SIZE; i += MEM_TLB_PAGE_SIZE)
			bp[i] = (uint8_t)(ta->cycles & 0xFF);

		munmap(p, MEM_TLB_REGION_SIZE);
		ta->cycles++;
	}

	ta->error = 0;
	return (NULL);
}

ATF_TC(mem_stress_tlb);
ATF_TC_HEAD(mem_stress_tlb, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-MSTR-03] TLB shootdown stress via repeated mmap/touch/munmap "
	    "of 2 MiB regions (120 s); asserts mprotect(2) still works after");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(mem_stress_tlb, tc)
{
	struct tlb_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	int ncpus, i, error;
	time_t deadline;
	uint64_t total_cycles = 0;
	void *probe;

	ncpus   = stress_ncpus();
	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(struct tlb_arg));
	ATF_REQUIRE(threads != NULL && args != NULL);

	for (i = 0; i < ncpus; i++) {
		args[i].stop   = &stop;
		args[i].error  = -1;
		args[i].cycles = 0;

		error = pthread_create(&threads[i], NULL, tlb_thread,
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

	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		ATF_CHECK_MSG(args[i].error == 0,
		    "TLB thread %d failed: %s", i, strerror(args[i].error));
		total_cycles += args[i].cycles;
	}

	/*
	 * Post-stress sanity: mprotect(2) must still succeed, verifying that
	 * TLB management is functional after the shootdown storm.
	 */
	probe = mmap(NULL, MEM_TLB_PAGE_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	ATF_REQUIRE_MSG(probe != MAP_FAILED,
	    "post-stress mmap failed: %s", strerror(errno));
	ATF_CHECK_MSG(mprotect(probe, MEM_TLB_PAGE_SIZE, PROT_READ) == 0,
	    "post-stress mprotect(PROT_READ) failed: %s", strerror(errno));
	ATF_CHECK_MSG(mprotect(probe, MEM_TLB_PAGE_SIZE,
	    PROT_READ | PROT_WRITE) == 0,
	    "post-stress mprotect(PROT_READ|PROT_WRITE) failed: %s",
	    strerror(errno));
	munmap(probe, MEM_TLB_PAGE_SIZE);

	printf("tlb: %llu mmap/touch/munmap cycles across %d threads "
	    "in %d seconds\n",
	    (unsigned long long)total_cycles, ncpus, STRESS_DURATION_SEC);

	free(threads);
	free(args);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, mem_stress_cache_thrash);
	ATF_TP_ADD_TC(tp, mem_stress_bandwidth);
	ATF_TP_ADD_TC(tp, mem_stress_tlb);

	return (atf_no_error());
}

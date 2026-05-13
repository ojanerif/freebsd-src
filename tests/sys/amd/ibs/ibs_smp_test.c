/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * IBS SMP (Symmetric Multi-Processing) Tests
 *
 * This test suite validates IBS behavior on multi-core systems:
 *
 * 1. ibs_smp_per_cpu_config:
 *    Configure IBS independently on each CPU and verify that each
 *    CPU's MSR state is isolated and correct.
 *
 * 2. ibs_smp_concurrent_sampling:
 *    Enable IBS on all CPUs simultaneously and verify that sampling
 *    can be activated across the system without interference.
 *
 * 3. ibs_smp_cpu_migration:
 *    Test IBS behavior when a process migrates between CPUs.
 *    IBS configuration is per-core, so migration should not
 *    affect the MSR state of the original CPU.
 *
 * Reference: AMD Processor Programming Reference (PPR), IBS chapter.
 * Linux kernel: arch/x86/events/amd/ibs.c
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/errno.h>
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

#include "ibs_utils.h"

/*
 * Helper: Get the number of online CPUs.
 */
static int
get_ncpus(void)
{
	long ncpus;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 1)
		ncpus = 1;

	return ((int)ncpus);
}

/*
 * Helper: Pin the current thread to a specific CPU.
 * Returns 0 on success, -1 on failure.
 */
static int
pin_to_cpu(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);

	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask) != 0)
		return (-1);

	return (0);
}

/*
 * Helper: Get the current CPU the thread is running on.
 * Returns the CPU ID, or -1 on failure.
 */
static int
get_current_cpu(void)
{
	cpuset_t mask;
	int i;

	CPU_ZERO(&mask);
	if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask) != 0)
		return (-1);

	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &mask))
			return (i);
	}

	return (-1);
}

/*
 * Test: ibs_smp_per_cpu_config
 *
 * Configure IBS independently on each CPU and verify that each
 * CPU's MSR state is isolated and correct.
 *
 * This test:
 * 1. Iterates over all online CPUs
 * 2. Pins to each CPU in turn
 * 3. Writes a unique period value to IBS Fetch CTL
 * 4. Reads back and verifies the value matches
 * 5. Restores the original value
 */
ATF_TC(ibs_smp_per_cpu_config);
ATF_TC_HEAD(ibs_smp_per_cpu_config, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Configure IBS independently on each CPU and verify isolation");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_smp_per_cpu_config, tc)
{
	int ncpus, cpu, error;
	uint64_t original, written, readback;
	uint64_t test_maxcnt;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ncpus = get_ncpus();
	if (ncpus <= 1)
		atf_tc_skip("Test requires multiple CPUs (found %d)", ncpus);

	for (cpu = 0; cpu < ncpus; cpu++) {
		/* Pin to this CPU */
		error = pin_to_cpu(cpu);
		if (error != 0) {
			atf_tc_skip("Cannot pin to CPU %d: %s",
			    cpu, strerror(errno));
		}

		/* Verify we are on the expected CPU */
		int current_cpu = get_current_cpu();
		if (current_cpu != cpu) {
			atf_tc_skip("Failed to pin to CPU %d (on CPU %d)",
			    cpu, current_cpu);
		}

		/* Read original MSR value */
		error = read_msr(cpu, MSR_IBS_FETCH_CTL, &original);
		if (error != 0) {
			atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL on CPU %d: %s",
			    cpu, strerror(error));
		}

		/* Write a unique period value per CPU.
		 * Retry up to 10 times: an in-flight IBS NMI (from active sampling
		 * before this test) can fire between write_msr and read_msr and
		 * re-arm the counter with the previous period.  Writing 0 first
		 * quiesces in-flight NMIs before each attempt.
		 */
		test_maxcnt = 0x100 + cpu;
		{
			int retry;
			for (retry = 0; retry < 10; retry++) {
				/* Quiesce: disable IBS fetch to drain any in-flight NMI. */
				(void)write_msr(cpu, MSR_IBS_FETCH_CTL, 0ULL);
				written = (original & ~IBS_MAXCNT_MASK) | test_maxcnt;
				written &= ~IBS_FETCH_ENABLE_BIT;
				error = write_msr(cpu, MSR_IBS_FETCH_CTL, written);
				ATF_REQUIRE_ERRNO(0, error == 0);
				error = read_msr(cpu, MSR_IBS_FETCH_CTL, &readback);
				ATF_REQUIRE_ERRNO(0, error == 0);
				if (ibs_get_maxcnt(readback) == test_maxcnt)
					break;
				/* NMI fired between write and read — yield and retry */
				sched_yield();
			}
		}
		ATF_CHECK_EQ(ibs_get_maxcnt(readback), test_maxcnt);

		/* Restore original value */
		error = write_msr(cpu, MSR_IBS_FETCH_CTL, original);
		ATF_REQUIRE_ERRNO(0, error == 0);
	}
}

/*
 * Thread argument structure for concurrent sampling test.
 */
struct smp_concurrent_arg {
	int cpu;
	int error;
	uint64_t original;
	bool has_original;
};

/*
 * Thread function: Enable IBS on a specific CPU.
 */
static void *
smp_concurrent_enable_thread(void *arg)
{
	struct smp_concurrent_arg *ca = arg;
	uint64_t val;
	int error;

	/* Pin to target CPU */
	if (pin_to_cpu(ca->cpu) != 0) {
		ca->error = errno;
		return (NULL);
	}

	/* Read original value */
	error = read_msr(ca->cpu, MSR_IBS_OP_CTL, &val);
	if (error != 0) {
		ca->error = error;
		return (NULL);
	}

	ca->original = val;
	ca->has_original = true;

	/* Enable IBS Op sampling with a moderate period */
	val = (val & ~IBS_MAXCNT_MASK) | 0x1000;  /* period = 0x10000 cycles */
	val |= IBS_OP_ENABLE_BIT;

	error = write_msr(ca->cpu, MSR_IBS_OP_CTL, val);
	if (error != 0) {
		ca->error = error;
		return (NULL);
	}

	ca->error = 0;
	return (NULL);
}

/*
 * Thread function: Disable IBS on a specific CPU.
 */
static void *
smp_concurrent_disable_thread(void *arg)
{
	struct smp_concurrent_arg *ca = arg;
	int error;

	if (!ca->has_original)
		return (NULL);

	/* Pin to target CPU */
	if (pin_to_cpu(ca->cpu) != 0) {
		ca->error = errno;
		return (NULL);
	}

	/* Restore original value (disables IBS) */
	error = write_msr(ca->cpu, MSR_IBS_OP_CTL, ca->original);
	if (error != 0) {
		ca->error = error;
		return (NULL);
	}

	ca->error = 0;
	return (NULL);
}

/*
 * Test: ibs_smp_concurrent_sampling
 *
 * Enable IBS on all CPUs simultaneously and verify that sampling
 * can be activated across the system without interference.
 *
 * This test:
 * 1. Creates one thread per CPU
 * 2. Each thread enables IBS Op sampling on its assigned CPU
 * 3. Verifies all threads succeeded
 * 4. Creates threads to disable IBS and restore state
 */
ATF_TC(ibs_smp_concurrent_sampling);
ATF_TC_HEAD(ibs_smp_concurrent_sampling, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Enable IBS on all CPUs simultaneously and verify operation");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_smp_concurrent_sampling, tc)
{
	int ncpus, i, error;
	pthread_t *threads;
	struct smp_concurrent_arg *args;
	bool any_enabled = false;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ncpus = get_ncpus();
	if (ncpus <= 1)
		atf_tc_skip("Test requires multiple CPUs (found %d)", ncpus);

	threads = calloc(ncpus, sizeof(pthread_t));
	ATF_REQUIRE(threads != NULL);

	args = calloc(ncpus, sizeof(struct smp_concurrent_arg));
	ATF_REQUIRE(args != NULL);

	/* Initialize and start enable threads */
	for (i = 0; i < ncpus; i++) {
		args[i].cpu = i;
		args[i].error = -1;
		args[i].has_original = false;
		args[i].original = 0;

		error = pthread_create(&threads[i], NULL,
		    smp_concurrent_enable_thread, &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}

	/* Wait for all enable threads */
	for (i = 0; i < ncpus; i++) {
		error = pthread_join(threads[i], NULL);
		ATF_REQUIRE_EQ(error, 0);
	}

	/* Check all threads succeeded */
	for (i = 0; i < ncpus; i++) {
		if (args[i].error == 0)
			any_enabled = true;
		else
			atf_tc_fail("Failed to enable IBS on CPU %d: %s",
			    args[i].cpu, strerror(args[i].error));
	}

	ATF_REQUIRE_MSG(any_enabled, "No CPUs had IBS enabled successfully");

	/* Now disable IBS on all CPUs */
	for (i = 0; i < ncpus; i++) {
		error = pthread_create(&threads[i], NULL,
		    smp_concurrent_disable_thread, &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}

	/* Wait for all disable threads */
	for (i = 0; i < ncpus; i++) {
		error = pthread_join(threads[i], NULL);
		ATF_REQUIRE_EQ(error, 0);
	}

	free(args);
	free(threads);
}

/*
 * Thread argument for CPU migration test.
 */
struct migration_arg {
	volatile int *done;
	volatile int *migrated_cpu;
	int target_cpu;
	int original_cpu;
	int error;
};

/*
 * Thread function: Test CPU migration behavior.
 * Pins to original CPU, reads MSR, migrates to target CPU,
 * reads MSR again, and verifies they are independent.
 */
static void *
smp_migration_thread(void *arg)
{
	struct migration_arg *ma = arg;
	uint64_t orig_val, target_val;
	int error;

	/* Pin to original CPU */
	if (pin_to_cpu(ma->original_cpu) != 0) {
		ma->error = errno;
		*ma->done = 1;
		return (NULL);
	}

	/*
	 * Disable IBS fetch on original CPU before reading the baseline
	 * to prevent an in-flight NMI from mutating MaxCnt between the
	 * read here and the verify_val comparison after migration.
	 */
	(void)write_msr(ma->original_cpu, MSR_IBS_FETCH_CTL, 0ULL);

	/* Read MSR on original CPU after quiescing */
	error = read_msr(ma->original_cpu, MSR_IBS_FETCH_CTL, &orig_val);
	if (error != 0) {
		ma->error = error;
		*ma->done = 1;
		return (NULL);
	}

	/* Migrate to target CPU */
	if (pin_to_cpu(ma->target_cpu) != 0) {
		ma->error = errno;
		*ma->done = 1;
		return (NULL);
	}

	*ma->migrated_cpu = ma->target_cpu;

	/* Read MSR on target CPU - should be independent */
	error = read_msr(ma->target_cpu, MSR_IBS_FETCH_CTL, &target_val);
	if (error != 0) {
		ma->error = error;
		*ma->done = 1;
		return (NULL);
	}

	/*
	 * Write a different value on target CPU and verify
	 * the original CPU's MSR is unaffected.
	 */
	uint64_t new_target_val = 0x200;
	error = write_msr(ma->target_cpu, MSR_IBS_FETCH_CTL, new_target_val);
	if (error != 0) {
		ma->error = error;
		*ma->done = 1;
		return (NULL);
	}

	/* Migrate back to original CPU */
	if (pin_to_cpu(ma->original_cpu) != 0) {
		ma->error = errno;
		*ma->done = 1;
		return (NULL);
	}

	/* Verify original CPU's MSR was not affected by target CPU write */
	uint64_t verify_val;
	error = read_msr(ma->original_cpu, MSR_IBS_FETCH_CTL, &verify_val);
	if (error != 0) {
		ma->error = error;
		*ma->done = 1;
		return (NULL);
	}

	/* The original CPU should still have its original value */
	if (ibs_get_maxcnt(verify_val) != ibs_get_maxcnt(orig_val)) {
		ma->error = EBUSY;  /* Indicate isolation failure */
		*ma->done = 1;
		return (NULL);
	}

	/* Restore target CPU's original value */
	error = write_msr(ma->target_cpu, MSR_IBS_FETCH_CTL, target_val);
	if (error != 0) {
		ma->error = error;
		*ma->done = 1;
		return (NULL);
	}

	ma->error = 0;
	*ma->done = 1;
	return (NULL);
}

/*
 * Test: ibs_smp_cpu_migration
 *
 * Test IBS behavior when a process migrates between CPUs.
 *
 * IBS configuration is per-core (stored in per-core MSRs), so:
 * - Migration should not affect the MSR state of the original CPU
 * - Each CPU maintains independent IBS configuration
 *
 * This test:
 * 1. Creates a thread pinned to CPU 0
 * 2. Reads IBS Fetch CTL on CPU 0
 * 3. Migrates the thread to CPU 1
 * 4. Writes a different value to CPU 1's IBS Fetch CTL
 * 5. Migrates back to CPU 0
 * 6. Verifies CPU 0's MSR was not affected
 */
ATF_TC(ibs_smp_cpu_migration);
ATF_TC_HEAD(ibs_smp_cpu_migration, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test IBS behavior when process migrates between CPUs");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_smp_cpu_migration, tc)
{
	int ncpus, error;
	pthread_t thread;
	struct migration_arg ma;
	volatile int done = 0;
	volatile int migrated_cpu = -1;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ncpus = get_ncpus();
	if (ncpus <= 1)
		atf_tc_skip("Test requires multiple CPUs (found %d)", ncpus);

	/* Set up migration test: CPU 0 -> CPU 1 */
	ma.done = &done;
	ma.migrated_cpu = &migrated_cpu;
	ma.original_cpu = 0;
	ma.target_cpu = 1;
	ma.error = -1;

	error = pthread_create(&thread, NULL, smp_migration_thread, &ma);
	ATF_REQUIRE_EQ(error, 0);

	/* Wait for thread to complete */
	error = pthread_join(thread, NULL);
	ATF_REQUIRE_EQ(error, 0);

	/* Check results */
	if (ma.error != 0) {
		if (ma.error == EBUSY)
			atf_tc_fail("IBS MSR isolation failed: "
			    "CPU %d write affected CPU %d",
			    ma.target_cpu, ma.original_cpu);
		else
			atf_tc_fail("Migration test failed: %s",
			    strerror(ma.error));
	}

	ATF_CHECK_EQ(migrated_cpu, 1);
}

/*
 * Register all test cases with the ATF test program.
 */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_smp_per_cpu_config);
	ATF_TP_ADD_TC(tp, ibs_smp_concurrent_sampling);
	ATF_TP_ADD_TC(tp, ibs_smp_cpu_migration);

	return (atf_no_error());
}

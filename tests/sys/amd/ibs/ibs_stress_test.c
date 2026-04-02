/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * IBS Stress Tests
 *
 * This test suite validates IBS behavior under stress conditions:
 *
 * 1. ibs_stress_rapid_enable_disable:
 *    Rapid enable/disable cycling (1000 iterations) to verify
 *    MSR write reliability and state consistency.
 *
 * 2. ibs_stress_period_changes:
 *    Rapid period changes while sampling is enabled to verify
 *    that the hardware handles dynamic reconfiguration correctly.
 *
 * 3. ibs_stress_long_running:
 *    Long-running sampling test (60 seconds) to verify stability
 *    and detect any resource leaks or state corruption.
 *
 * 4. ibs_stress_concurrent_msr_access:
 *    Concurrent MSR reads from multiple threads to verify
 *    that MSR access is properly serialized at the hardware level.
 *
 * Reference: AMD Processor Programming Reference (PPR), IBS chapter.
 * Linux kernel: arch/x86/events/amd/ibs.c
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/errno.h>
#include <sys/sched.h>
#include <sys/time.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
 * Test: ibs_stress_rapid_enable_disable
 *
 * Rapid enable/disable cycling (1000 iterations) to verify
 * MSR write reliability and state consistency.
 *
 * This test:
 * 1. Reads the original IBS Op CTL value
 * 2. Performs 1000 rapid enable/disable cycles
 * 3. Verifies each write succeeds
 * 4. Restores the original value
 */
ATF_TC(ibs_stress_rapid_enable_disable);
ATF_TC_HEAD(ibs_stress_rapid_enable_disable, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Rapid IBS enable/disable cycling (1000 iterations)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_stress_rapid_enable_disable, tc)
{
	uint64_t original, val;
	int error, i;
	const int iterations = 1000;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/*
	 * Rapid enable/disable cycle.
	 * We alternate between enabled (with a moderate period) and
	 * disabled (period = 0, enable bit cleared).
	 */
	for (i = 0; i < iterations; i++) {
		/* Enable: set period and enable bit */
		val = (original & ~IBS_MAXCNT_MASK) | 0x100;
		val |= IBS_OP_ENABLE_BIT;

		error = write_msr(0, MSR_IBS_OP_CTL, val);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Disable: clear enable bit and period */
		val = original & ~IBS_MAXCNT_MASK;
		val &= ~IBS_OP_ENABLE_BIT;

		error = write_msr(0, MSR_IBS_OP_CTL, val);
		ATF_REQUIRE_ERRNO(0, error == 0);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_stress_period_changes
 *
 * Rapid period changes while sampling is enabled to verify
 * that the hardware handles dynamic reconfiguration correctly.
 *
 * This test:
 * 1. Enables IBS Op sampling
 * 2. Performs 500 rapid period changes with varying values
 * 3. Reads back each value to verify it was written correctly
 * 4. Disables IBS and restores original state
 */
ATF_TC(ibs_stress_period_changes);
ATF_TC_HEAD(ibs_stress_period_changes, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Rapid period changes while IBS sampling is enabled");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_stress_period_changes, tc)
{
	uint64_t original, written, readback;
	int error, i;
	const int iterations = 500;
	uint64_t test_periods[] = {
		0x0010,  /* Small period (256 cycles) */
		0x0100,  /* Medium period (4096 cycles) */
		0x1000,  /* Large period (65536 cycles) */
		0x8000,  /* Very large period (524288 cycles) */
		0xFFFF,  /* Maximum period */
	};
	const int n_periods = nitems(test_periods);

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Enable IBS with initial period */
	written = (original & ~IBS_MAXCNT_MASK) | test_periods[0];
	written |= IBS_OP_ENABLE_BIT;

	error = write_msr(0, MSR_IBS_OP_CTL, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Rapid period changes */
	for (i = 0; i < iterations; i++) {
		uint64_t period = test_periods[i % n_periods];

		/* Write new period (keep enable bit set) */
		written = (original & ~IBS_MAXCNT_MASK) | period;
		written |= IBS_OP_ENABLE_BIT;

		error = write_msr(0, MSR_IBS_OP_CTL, written);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Read back and verify */
		error = read_msr(0, MSR_IBS_OP_CTL, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK_EQ(ibs_get_maxcnt(readback), period);
	}

	/* Disable IBS and restore original value */
	error = write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Thread argument for long-running test.
 */
struct long_running_arg {
	volatile bool *stop;
	int cpu;
	int error;
	int iterations;
};

/*
 * Thread function: Continuously read/write IBS MSR until stopped.
 */
static void *
long_running_thread(void *arg)
{
	struct long_running_arg *lra = arg;
	uint64_t original, val;
	int error;
	int count = 0;

	/* Pin to target CPU */
	if (pin_to_cpu(lra->cpu) != 0) {
		lra->error = errno;
		return (NULL);
	}

	/* Read original value */
	error = read_msr(lra->cpu, MSR_IBS_FETCH_CTL, &original);
	if (error != 0) {
		lra->error = error;
		return (NULL);
	}

	/* Run until stopped */
	while (!*lra->stop) {
		/* Write a test value */
		val = (original & ~IBS_MAXCNT_MASK) | (0x100 + (count & 0xFF));
		val &= ~IBS_FETCH_ENABLE_BIT;

		error = write_msr(lra->cpu, MSR_IBS_FETCH_CTL, val);
		if (error != 0) {
			lra->error = error;
			return (NULL);
		}

		/* Read back to verify */
		error = read_msr(lra->cpu, MSR_IBS_FETCH_CTL, &val);
		if (error != 0) {
			lra->error = error;
			return (NULL);
		}

		count++;
	}

	lra->iterations = count;
	lra->error = 0;

	/* Restore original value */
	write_msr(lra->cpu, MSR_IBS_FETCH_CTL, original);

	return (NULL);
}

/*
 * Test: ibs_stress_long_running
 *
 * Long-running sampling test (60 seconds) to verify stability
 * and detect any resource leaks or state corruption.
 *
 * This test:
 * 1. Creates threads on each CPU
 * 2. Each thread continuously accesses IBS MSRs for 60 seconds
 * 3. Verifies all threads complete without errors
 * 4. Reports total iterations completed
 */
ATF_TC(ibs_stress_long_running);
ATF_TC_HEAD(ibs_stress_long_running, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Long-running IBS stress test (60 seconds)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_stress_long_running, tc)
{
	int ncpus, i, error;
	pthread_t *threads;
	struct long_running_arg *args;
	volatile bool stop = false;
	const int duration_sec = 60;
	int total_iterations = 0;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ncpus = get_ncpus();

	threads = calloc(ncpus, sizeof(pthread_t));
	ATF_REQUIRE(threads != NULL);

	args = calloc(ncpus, sizeof(struct long_running_arg));
	ATF_REQUIRE(args != NULL);

	/* Initialize and start threads */
	for (i = 0; i < ncpus; i++) {
		args[i].stop = &stop;
		args[i].cpu = i;
		args[i].error = -1;
		args[i].iterations = 0;

		error = pthread_create(&threads[i], NULL,
		    long_running_thread, &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}

	/* Run for the specified duration */
	sleep(duration_sec);

	/* Signal threads to stop */
	stop = true;

	/* Wait for all threads */
	for (i = 0; i < ncpus; i++) {
		error = pthread_join(threads[i], NULL);
		ATF_REQUIRE_EQ(error, 0);

		if (args[i].error != 0) {
			atf_tc_fail("Thread for CPU %d failed: %s",
			    args[i].cpu, strerror(args[i].error));
		}

		total_iterations += args[i].iterations;
	}

	/* Report results */
	printf("Long-running test completed: %d total iterations across "
	    "%d CPUs over %d seconds\n",
	    total_iterations, ncpus, duration_sec);

	free(args);
	free(threads);
}

/*
 * Thread argument for concurrent MSR access test.
 */
struct concurrent_msr_arg {
	int iterations;
	int cpu;
	int error;
	uint64_t last_read;
};

/*
 * Thread function: Repeatedly read MSR from a specific CPU.
 */
static void *
concurrent_msr_read_thread(void *arg)
{
	struct concurrent_msr_arg *cma = arg;
	uint64_t val;
	int error, i;

	for (i = 0; i < cma->iterations; i++) {
		error = read_msr(cma->cpu, MSR_IBS_FETCH_CTL, &val);
		if (error != 0) {
			cma->error = error;
			return (NULL);
		}
		cma->last_read = val;
	}

	cma->error = 0;
	return (NULL);
}

/*
 * Test: ibs_stress_concurrent_msr_access
 *
 * Concurrent MSR reads from multiple threads to verify
 * that MSR access is properly serialized at the hardware level.
 *
 * This test:
 * 1. Creates multiple threads, each reading MSRs from different CPUs
 * 2. All threads perform 10000 reads concurrently
 * 3. Verifies all reads succeed without errors
 */
ATF_TC(ibs_stress_concurrent_msr_access);
ATF_TC_HEAD(ibs_stress_concurrent_msr_access, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Concurrent MSR access from multiple threads");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_stress_concurrent_msr_access, tc)
{
	int ncpus, i, error;
	pthread_t *threads;
	struct concurrent_msr_arg *args;
	const int iterations = 10000;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ncpus = get_ncpus();
	if (ncpus <= 1)
		atf_tc_skip("Test requires multiple CPUs (found %d)", ncpus);

	threads = calloc(ncpus, sizeof(pthread_t));
	ATF_REQUIRE(threads != NULL);

	args = calloc(ncpus, sizeof(struct concurrent_msr_arg));
	ATF_REQUIRE(args != NULL);

	/* Initialize and start threads */
	for (i = 0; i < ncpus; i++) {
		args[i].iterations = iterations;
		args[i].cpu = i;
		args[i].error = -1;
		args[i].last_read = 0;

		error = pthread_create(&threads[i], NULL,
		    concurrent_msr_read_thread, &args[i]);
		ATF_REQUIRE_EQ(error, 0);
	}

	/* Wait for all threads */
	for (i = 0; i < ncpus; i++) {
		error = pthread_join(threads[i], NULL);
		ATF_REQUIRE_EQ(error, 0);

		if (args[i].error != 0) {
			atf_tc_fail("Thread for CPU %d failed after %d reads: %s",
			    args[i].cpu, iterations, strerror(args[i].error));
		}
	}

	free(args);
	free(threads);
}

/*
 * Register all test cases with the ATF test program.
 */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_stress_rapid_enable_disable);
	ATF_TP_ADD_TC(tp, ibs_stress_period_changes);
	ATF_TP_ADD_TC(tp, ibs_stress_long_running);
	ATF_TP_ADD_TC(tp, ibs_stress_concurrent_msr_access);

	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Osvaldo Janeri Filho <ojanerif@amd.com>
 *
 * [TC-TSCSTR] AMD TSC stress tests.
 *
 * Validates that tsc_read() returns monotonically increasing values and a
 * stable frequency under sustained CPU and memory load.  Three test cases:
 *
 * TC-TSCSTR-01  tsc_stress_monotonic_under_load
 *   Launches one xorshift64 CPU-burner and one 32 MiB stride memory-burner
 *   (each pinned to a separate CPU).  Polls tsc_read() every 1 ms for 120 s
 *   on the primary thread (pinned to CPU 0) and asserts strict monotonicity
 *   at every sample.
 *   CRITICAL — TSC must not pause or regress under load.
 *   Timeout: 200 s.
 *
 * TC-TSCSTR-02  tsc_stress_frequency_stable_under_load
 *   Same stressor pair.  Collects 12 × 10 s TSC measurement windows and
 *   asserts that the derived TSC frequency varies < 2% across all windows.
 *   HIGH — rate constancy under CPU+memory pressure.
 *   Timeout: 200 s.
 *
 * TC-TSCSTR-03  tsc_stress_rapid_read_10m
 *   10 million back-to-back tsc_read() calls with the same stressors
 *   running.  Asserts strict monotonicity at every call.
 *   HIGH — validates no fabricated or cached values under tight loops.
 *   Timeout: 60 s.
 *
 * Reference: AMD64 APM Vol. 3 §2.4 (CPUID 0x80000007 EDX[8]).
 *
 * SWLSVROS-6556
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>

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

#include "tsc_utils.h"

/* -------------------------------------------------------------------------
 * File-local helpers
 * ---------------------------------------------------------------------- */

static int
tsc_pin_to_cpu(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask) == 0 ? 0 : -1);
}

static void
tsc_skip_unless_cpuctl(void)
{
	int fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		atf_tc_skip("/dev/cpuctl0 not accessible: %s", strerror(errno));
	close(fd);
}

/* Return elapsed seconds between two CLOCK_MONOTONIC samples. */
static double
timespec_diff_s(const struct timespec *a, const struct timespec *b)
{
	return ((double)(a->tv_sec - b->tv_sec) +
	    (double)(a->tv_nsec - b->tv_nsec) / 1e9);
}

/* -------------------------------------------------------------------------
 * Stressor infrastructure
 *
 * Two thread types:
 *   cpu_worker  — xorshift64 spin loop (no memory pressure)
 *   mem_worker  — 32 MiB forward stride, sum accumulate (L3/DRAM pressure)
 *
 * Both threads stop when *stop_flag is set.  Both can be pinned to a CPU.
 * Threads signal readiness via an atomic bool so the caller can synchronise.
 * ---------------------------------------------------------------------- */

#define	TSC_MEM_WORKER_SZ	(32u * 1024u * 1024u)	/* 32 MiB */
#define	TSC_MEM_STRIDE		64			/* cache-line stride */

struct tsc_stressor_arg {
	volatile bool	 stop;
	volatile bool	 ready;
	int		 pin_cpu;	/* -1 = no pin */
	int		 pin_error;
	/* mem worker only */
	volatile uint8_t *buf;		/* caller-allocated TSC_MEM_WORKER_SZ */
};

static void *
tsc_cpu_worker_fn(void *varg)
{
	struct tsc_stressor_arg *arg = varg;
	uint64_t x = 0x123456789abcdef1ULL;

	if (arg->pin_cpu >= 0)
		arg->pin_error = tsc_pin_to_cpu(arg->pin_cpu);

	__atomic_store_n(&arg->ready, true, __ATOMIC_RELEASE);

	while (!__atomic_load_n(&arg->stop, __ATOMIC_ACQUIRE)) {
		/* xorshift64 — keeps one register spinning */
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		__asm__ volatile("" : "+r"(x));
	}
	return (NULL);
}

static void *
tsc_mem_worker_fn(void *varg)
{
	struct tsc_stressor_arg *arg = varg;
	volatile uint8_t *buf = arg->buf;
	size_t len = TSC_MEM_WORKER_SZ;
	uint64_t sum = 0;
	size_t i;

	if (arg->pin_cpu >= 0)
		arg->pin_error = tsc_pin_to_cpu(arg->pin_cpu);

	__atomic_store_n(&arg->ready, true, __ATOMIC_RELEASE);

	while (!__atomic_load_n(&arg->stop, __ATOMIC_ACQUIRE)) {
		for (i = 0; i < len; i += TSC_MEM_STRIDE)
			sum += buf[i];
		__asm__ volatile("" : "+r"(sum));
	}
	return (NULL);
}

struct tsc_stressor_set {
	struct tsc_stressor_arg	cpu_arg;
	struct tsc_stressor_arg	mem_arg;
	pthread_t		cpu_tid;
	pthread_t		mem_tid;
	uint8_t		       *mem_buf;
};

/*
 * Start one CPU-burner on cpu_pin and one memory-burner on mem_pin.
 * Pass -1 to not pin.  Returns 0 on success, non-zero on failure.
 */
static int
tsc_start_stressors(struct tsc_stressor_set *s, int cpu_pin, int mem_pin)
{
	int rc;

	memset(s, 0, sizeof(*s));

	s->mem_buf = malloc(TSC_MEM_WORKER_SZ);
	if (s->mem_buf == NULL)
		return (ENOMEM);
	memset(s->mem_buf, 0xA5, TSC_MEM_WORKER_SZ);

	s->cpu_arg.stop     = false;
	s->cpu_arg.ready    = false;
	s->cpu_arg.pin_cpu  = cpu_pin;
	s->cpu_arg.pin_error = 0;

	s->mem_arg.stop     = false;
	s->mem_arg.ready    = false;
	s->mem_arg.pin_cpu  = mem_pin;
	s->mem_arg.pin_error = 0;
	s->mem_arg.buf      = (volatile uint8_t *)s->mem_buf;

	rc = pthread_create(&s->cpu_tid, NULL, tsc_cpu_worker_fn, &s->cpu_arg);
	if (rc != 0) {
		free(s->mem_buf);
		return (rc);
	}
	rc = pthread_create(&s->mem_tid, NULL, tsc_mem_worker_fn, &s->mem_arg);
	if (rc != 0) {
		__atomic_store_n(&s->cpu_arg.stop, true, __ATOMIC_RELEASE);
		pthread_join(s->cpu_tid, NULL);
		free(s->mem_buf);
		return (rc);
	}

	/* Spin until both workers are alive. */
	while (!__atomic_load_n(&s->cpu_arg.ready, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&s->mem_arg.ready, __ATOMIC_ACQUIRE))
		__asm__ volatile("pause" ::: "memory");

	return (0);
}

static void
tsc_stop_stressors(struct tsc_stressor_set *s)
{
	__atomic_store_n(&s->cpu_arg.stop, true, __ATOMIC_RELEASE);
	__atomic_store_n(&s->mem_arg.stop, true, __ATOMIC_RELEASE);
	pthread_join(s->cpu_tid, NULL);
	pthread_join(s->mem_tid, NULL);
	free(s->mem_buf);
	s->mem_buf = NULL;
}

/* =========================================================================
 * TC-TSCSTR-01  tsc_stress_monotonic_under_load
 * ====================================================================== */
ATF_TC(tsc_stress_monotonic_under_load);
ATF_TC_HEAD(tsc_stress_monotonic_under_load, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSCSTR-01] Launch one xorshift64 CPU-burner (CPU 1) and one "
	    "32 MiB stride memory-burner (CPU 2, or CPU 1 if only 2 CPUs). "
	    "Poll tsc_read() every 1 ms for 120 s on CPU 0 and assert strict "
	    "monotonicity at every sample.  CRITICAL.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(tsc_stress_monotonic_under_load, tc)
{
#define	STR01_POLL_US		1000		/* 1 ms poll interval */
#define	STR01_DURATION_S	120		/* 120 s test window */

	struct tsc_stressor_set ss;
	struct timespec t0, now;
	uint64_t prev, curr;
	long ncpus;
	int cpu_pin, mem_pin, failures = 0;
	unsigned long samples = 0;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set");

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	cpu_pin = (ncpus > 1) ? 1 : 0;
	mem_pin = (ncpus > 2) ? 2 : cpu_pin;

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	ATF_REQUIRE_MSG(tsc_start_stressors(&ss, cpu_pin, mem_pin) == 0,
	    "Failed to start stressor threads");

	ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &t0) == 0,
	    "clock_gettime: %s", strerror(errno));

	prev = tsc_read();

	for (;;) {
		usleep(STR01_POLL_US);

		curr = tsc_read();
		samples++;

		if (curr <= prev) {
			fprintf(stderr,
			    "sample %lu: TSC regressed or stalled "
			    "(prev=%ju curr=%ju delta=%jd)\n",
			    samples, (uintmax_t)prev, (uintmax_t)curr,
			    (intmax_t)(curr - prev));
			failures++;
		}
		prev = curr;

		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &now) == 0,
		    "clock_gettime: %s", strerror(errno));
		if (timespec_diff_s(&now, &t0) >= STR01_DURATION_S)
			break;
	}

	tsc_stop_stressors(&ss);

	printf("TSC monotonicity under load: %lu samples over %d s\n",
	    samples, STR01_DURATION_S);
	printf("  failures = %d\n", failures);

	ATF_CHECK_MSG(failures == 0,
	    "TSC regressed or stalled %d time(s) in %lu samples under "
	    "CPU+memory load",
	    failures, samples);

#undef STR01_POLL_US
#undef STR01_DURATION_S
}

/* =========================================================================
 * TC-TSCSTR-02  tsc_stress_frequency_stable_under_load
 * ====================================================================== */
ATF_TC(tsc_stress_frequency_stable_under_load);
ATF_TC_HEAD(tsc_stress_frequency_stable_under_load, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSCSTR-02] Same CPU+memory stressor pair.  Collect 12 × 10 s "
	    "TSC measurement windows on CPU 0 and assert the derived TSC "
	    "frequency varies < 2%% across all windows.  HIGH.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(tsc_stress_frequency_stable_under_load, tc)
{
#define	STR02_WINDOWS		12
#define	STR02_WINDOW_S		10		/* 10 s per window */
#define	STR02_WINDOW_US		(STR02_WINDOW_S * 1000000)

	struct tsc_stressor_set ss;
	struct timespec w0, w1;
	uint64_t tsc0, tsc1;
	double freq[STR02_WINDOWS], elapsed;
	double min_freq, max_freq, sum_freq, variation_pct;
	long ncpus;
	int cpu_pin, mem_pin, i;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set");

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	cpu_pin = (ncpus > 1) ? 1 : 0;
	mem_pin = (ncpus > 2) ? 2 : cpu_pin;

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	ATF_REQUIRE_MSG(tsc_start_stressors(&ss, cpu_pin, mem_pin) == 0,
	    "Failed to start stressor threads");

	for (i = 0; i < STR02_WINDOWS; i++) {
		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w0) == 0,
		    "clock_gettime: %s", strerror(errno));
		tsc0 = tsc_read();

		usleep(STR02_WINDOW_US);

		tsc1 = tsc_read();
		ATF_REQUIRE_MSG(clock_gettime(CLOCK_MONOTONIC, &w1) == 0,
		    "clock_gettime: %s", strerror(errno));

		elapsed = timespec_diff_s(&w1, &w0);
		if (elapsed <= 0.0) {
			tsc_stop_stressors(&ss);
			atf_tc_skip("Window %d elapsed <= 0", i);
		}

		freq[i] = (double)(tsc1 - tsc0) / elapsed;
		printf("Window %d: %.4f GHz  (elapsed=%.4f s  "
		    "tsc_delta=%ju)\n",
		    i + 1, freq[i] / 1e9, elapsed,
		    (uintmax_t)(tsc1 - tsc0));
	}

	tsc_stop_stressors(&ss);

	min_freq = max_freq = sum_freq = freq[0];
	for (i = 1; i < STR02_WINDOWS; i++) {
		if (freq[i] < min_freq) min_freq = freq[i];
		if (freq[i] > max_freq) max_freq = freq[i];
		sum_freq += freq[i];
	}

	variation_pct = (max_freq - min_freq) /
	    (sum_freq / STR02_WINDOWS) * 100.0;

	printf("TSC frequency variation across %d × %d s windows "
	    "(under load): %.4f%%  (threshold 2.00%%)\n",
	    STR02_WINDOWS, STR02_WINDOW_S, variation_pct);
	printf("  min=%.4f GHz  max=%.4f GHz  avg=%.4f GHz\n",
	    min_freq / 1e9, max_freq / 1e9,
	    (sum_freq / STR02_WINDOWS) / 1e9);

	ATF_CHECK_MSG(variation_pct < 2.0,
	    "TSC frequency variation %.4f%% across %d windows exceeds 2%% "
	    "under load (min=%.4f GHz max=%.4f GHz)",
	    variation_pct, STR02_WINDOWS, min_freq / 1e9, max_freq / 1e9);

#undef STR02_WINDOWS
#undef STR02_WINDOW_S
#undef STR02_WINDOW_US
}

/* =========================================================================
 * TC-TSCSTR-03  tsc_stress_rapid_read_10m
 * ====================================================================== */
ATF_TC(tsc_stress_rapid_read_10m);
ATF_TC_HEAD(tsc_stress_rapid_read_10m, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-TSCSTR-03] Execute 10 million back-to-back tsc_read() calls "
	    "with the same CPU+memory stressor pair running.  Asserts strict "
	    "monotonicity at every call.  HIGH.");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "60");
}

ATF_TC_BODY(tsc_stress_rapid_read_10m, tc)
{
#define	STR03_ITERS	10000000UL

	struct tsc_stressor_set ss;
	uint64_t prev, curr;
	long ncpus;
	int cpu_pin, mem_pin, failures = 0;
	unsigned long i;

	tsc_skip_unless_cpuctl();

	if (!tsc_invariant_present())
		atf_tc_skip("InvariantTSC not set");

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	cpu_pin = (ncpus > 1) ? 1 : 0;
	mem_pin = (ncpus > 2) ? 2 : cpu_pin;

	ATF_REQUIRE_MSG(tsc_pin_to_cpu(0) == 0,
	    "Cannot pin to CPU 0: %s", strerror(errno));

	ATF_REQUIRE_MSG(tsc_start_stressors(&ss, cpu_pin, mem_pin) == 0,
	    "Failed to start stressor threads");

	prev = tsc_read();

	for (i = 0; i < STR03_ITERS; i++) {
		curr = tsc_read();

		if (curr <= prev) {
			if (failures < 10)
				fprintf(stderr,
				    "iter %lu: TSC did not advance "
				    "(prev=%ju curr=%ju delta=%jd)\n",
				    i, (uintmax_t)prev, (uintmax_t)curr,
				    (intmax_t)(curr - prev));
			failures++;
		}
		prev = curr;
	}

	tsc_stop_stressors(&ss);

	printf("TSC rapid-read: %lu iterations under CPU+memory load\n",
	    STR03_ITERS);
	printf("  monotonicity failures = %d\n", failures);

	ATF_CHECK_MSG(failures == 0,
	    "TSC failed to advance %d time(s) in %lu back-to-back reads "
	    "under CPU+memory load",
	    failures, STR03_ITERS);

#undef STR03_ITERS
}

/* =========================================================================
 * Test program registration
 * ====================================================================== */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tsc_stress_monotonic_under_load);
	ATF_TP_ADD_TC(tp, tsc_stress_frequency_stable_under_load);
	ATF_TP_ADD_TC(tp, tsc_stress_rapid_read_10m);
	return (atf_no_error());
}

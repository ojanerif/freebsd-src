/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

#ifndef _STRESS_UTILS_H_
#define _STRESS_UTILS_H_

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sched.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Default stress duration (seconds) for ATF test cases. */
#define STRESS_DURATION_SEC		120

/* MSR polling / progress-print interval. */
#define STRESS_POLL_INTERVAL_SEC	5

/*
 * Return the number of online CPUs.
 */
static inline int
stress_ncpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return ((int)(n < 1 ? 1 : n));
}

/*
 * Return the current wall-clock time in milliseconds.
 */
static inline uint64_t
stress_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000ULL +
	    (uint64_t)(ts.tv_nsec / 1000000LL));
}

/*
 * Write a unique temp file path for test PID into buf[len].
 * Caller must unlink the file in the ATF cleanup body.
 */
static inline void
stress_tmpfile_path(char *buf, size_t len, const char *label)
{
	snprintf(buf, len, "/tmp/stress_%s_%d", label, (int)getpid());
}

/*
 * xorshift64 step — pure integer arithmetic.
 */
#define STRESS_XORSHIFT64(x)	do {	\
	(x) ^= (x) << 13;		\
	(x) ^= (x) >> 7;		\
	(x) ^= (x) << 17;		\
} while (0)

/* LCG multiplier for random-access patterns. */
#define STRESS_LCG_MUL		6364136223846793005ULL

#endif /* _STRESS_UTILS_H_ */

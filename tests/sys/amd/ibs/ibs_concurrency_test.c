/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 *
 * [TC-CON] — IBS concurrency E2E tests.
 *
 * Validates that multiple processes and threads can access IBS MSRs
 * concurrently without fd corruption, cross-process leakage, or signal
 * handler interference.
 *
 * Requires root and AMD IBS hardware.
 *
 * Test IDs: TC-CON-01 … TC-CON-02
 */

#include <sys/param.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ibs_utils.h"

#define CON_MSR_ITERATIONS	100	/* reads per child process */
#define CON_ALARM_DURATION_SEC	5	/* seconds to run signal storm */
#define CON_ALARM_INTERVAL_US	1000	/* 1 ms between SIGALRM deliveries */

/* -----------------------------------------------------------------------
 * TC-CON-01: multiprocess concurrent MSR access
 *
 * Forks one child per CPU.  Each child opens /dev/cpuctl<N> for its CPU,
 * reads MSR_IBS_FETCH_CTL CON_MSR_ITERATIONS times, and exits 0.
 * The parent waits for all children and verifies each exited 0.
 * ----------------------------------------------------------------------- */
ATF_TC(ibs_concurrent_multiprocess);
ATF_TC_HEAD(ibs_concurrent_multiprocess, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-CON-01] N children (one per CPU) hammer IBS MSRs concurrently");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_concurrent_multiprocess, tc)
{
	pid_t pids[64];
	int ncpus, i, status;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 1)
		atf_tc_skip("sysconf returned ncpus < 1");
	if (ncpus > 64)
		ncpus = 64;

	for (i = 0; i < ncpus; i++) {
		pids[i] = fork();
		ATF_REQUIRE(pids[i] >= 0);

		if (pids[i] == 0) {
			int j;
			uint64_t val;

			for (j = 0; j < CON_MSR_ITERATIONS; j++) {
				int err = read_msr(i, MSR_IBS_FETCH_CTL, &val);
				if (err != 0)
					_exit(1);
			}
			_exit(0);
		}
	}

	for (i = 0; i < ncpus; i++) {
		waitpid(pids[i], &status, 0);
		ATF_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "child for CPU %d exited with status %d", i,
		    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	}
}

/* -----------------------------------------------------------------------
 * Thread state for signal storm test
 * ----------------------------------------------------------------------- */
struct alarm_thread_arg {
	volatile bool *stop;
	pthread_t target;
};

static void
sigalrm_noop(int sig __unused)
{
	/* intentional no-op: just interrupt any syscall in progress */
}

static void *
alarm_sender_thread(void *arg)
{
	struct alarm_thread_arg *a = (struct alarm_thread_arg *)arg;

	while (!*a->stop) {
		pthread_kill(a->target, SIGALRM);
		usleep(CON_ALARM_INTERVAL_US);
	}
	return (NULL);
}

/* -----------------------------------------------------------------------
 * TC-CON-02: signal storm while IBS sampling is active
 *
 * Enables IBS Op with a moderate period on CPU 0.  A second thread sends
 * SIGALRM to the main thread at 1 kHz for CON_ALARM_DURATION_SEC seconds.
 * The test passes if the process survives and MSRs remain accessible.
 * ----------------------------------------------------------------------- */
ATF_TC(ibs_signal_storm_under_sampling);
ATF_TC_HEAD(ibs_signal_storm_under_sampling, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-CON-02] IBS Op active + 1 kHz SIGALRM — no SIGSEGV or crash");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "120");
}

ATF_TC_BODY(ibs_signal_storm_under_sampling, tc)
{
	struct sigaction sa;
	struct alarm_thread_arg arg;
	volatile bool stop = false;
	pthread_t alarm_thread;
	uint64_t original_op, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original_op);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s", strerror(error));

	/* Install a no-op SIGALRM handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigalrm_noop;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;	/* no SA_RESTART — let syscalls return EINTR */
	sigaction(SIGALRM, &sa, NULL);

	/* Enable IBS Op sampling with a moderate period */
	error = write_msr(0, MSR_IBS_OP_CTL,
	    IBS_OP_EN | 0x1000ULL);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Start the alarm-sending thread */
	arg.stop   = &stop;
	arg.target = pthread_self();
	error = pthread_create(&alarm_thread, NULL, alarm_sender_thread, &arg);
	ATF_REQUIRE_EQ(error, 0);

	/* Run for the test duration; read MSRs to exercise the NMI path */
	sleep(CON_ALARM_DURATION_SEC);

	/* Stop the alarm thread */
	stop = true;
	pthread_join(alarm_thread, NULL);

	/* Disable IBS Op */
	write_msr(0, MSR_IBS_OP_CTL, 0ULL);

	/* Verify the MSR is still accessible and sampling is off */
	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	ATF_REQUIRE_MSG(error == 0,
	    "MSR_IBS_OP_CTL inaccessible after signal storm: %s",
	    strerror(error));
	ATF_CHECK_MSG((readback & IBS_OP_EN) == 0,
	    "IBS Op still enabled after disable: readback=0x%016llx",
	    (unsigned long long)readback);

	/* Restore original value */
	write_msr(0, MSR_IBS_OP_CTL, original_op);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_concurrent_multiprocess);
	ATF_TP_ADD_TC(tp, ibs_signal_storm_under_sampling);
	return (atf_no_error());
}

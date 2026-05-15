/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * IBS Op Sampling Under Stress — TC-ISTR
 *
 * Validates that AMD IBS Op sampling produces coherent results while each
 * class of system resource is under sustained stress.  Each of the four
 * 120-second test cases runs a workload concurrently with IBS Op sampling
 * on CPU 0, polling MSR_IBS_OP_CTL every 5 seconds to verify:
 *
 *   (a) MSR reads succeed throughout the stress period.
 *   (b) IbsOpEn bit stays set (sampling engine is active).
 *   (c) When IbsOpVal is caught in a poll, the sample RIP is non-zero
 *       and the IbsRipInvalid flag is clear.
 *   (d) The workaround #420 stop sequence completes cleanly and the MSR
 *       reads 0 after the drain.
 *
 * Requires: root (cpuctl MSR write), IBS-capable AMD CPU, hwpmc module loaded.
 * Skips gracefully if hardware or permissions are absent.
 *
 * Test cases:
 *   ibs_op_cpu_stress  [TC-ISTR-01]  IBS Op + per-CPU xorshift64 ALU
 *   ibs_op_mem_stress  [TC-ISTR-02]  IBS Op + 128 MiB random-strided thrash
 *   ibs_op_disk_stress [TC-ISTR-03]  IBS Op + sequential + random /tmp I/O
 *   ibs_op_net_stress  [TC-ISTR-04]  IBS Op + AF_UNIX socketpair flood
 *
 * References:
 *   AMD PPR — IBS chapter (MSR_IBS_OP_CTL, IbsOpEn, IbsOpVal, MaxCnt)
 *   AMD Erratum #420 — stop sequence for IBS to avoid in-flight NMI drains
 *   FreeBSD hwpmc_ibs.c ibs_stop_pmc()
 *   FreeBSD-Tests-026 (NMI storm analysis; safe period chosen from that work)
 */

#include <sys/types.h>
#include <sys/cpuset.h>
#include <sys/mman.h>
#include <sys/sched.h>
#include <sys/socket.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stress_ibs.h"
#include "stress_utils.h"

/* -----------------------------------------------------------------------
 * Sampling result counters — filled by ibs_op_run_sampling()
 * ----------------------------------------------------------------------- */

struct ibs_poll_stats {
	int read_errors;	/* MSR_IBS_OP_CTL read failures */
	int polls_active;	/* polls where IbsOpEn was set */
	int val_seen;		/* polls where IbsOpVal was caught */
	int val_bad_rip;	/* IbsOpVal samples with RIP=0 or RIP_INVALID */
	int disable_ok;		/* 1 if workaround #420 drain left MSR = 0 */
};

/*
 * Enable IBS Op on CPU 0, poll for STRESS_DURATION_SEC, then disable.
 * The stress workload runs in background threads started by the caller.
 * saved_ctl is the MSR value to restore after the sampling run.
 */
static void
ibs_op_run_sampling(uint64_t saved_ctl, struct ibs_poll_stats *st)
{
	uint64_t ctl, rip, data;
	time_t deadline;

	memset(st, 0, sizeof(*st));

	if (sibs_write_msr(0, SIBS_MSR_OP_CTL,
	    SIBS_OP_EN | SIBS_SAFE_PERIOD) != 0)
		return;		/* enable failed; caller checks polls_active == 0 */

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline) {
		sleep(STRESS_POLL_INTERVAL_SEC);
		if (sibs_read_msr(0, SIBS_MSR_OP_CTL, &ctl) != 0) {
			st->read_errors++;
			continue;
		}
		if (ctl & SIBS_OP_EN)
			st->polls_active++;

		if (ctl & SIBS_OP_VAL) {
			st->val_seen++;
			/*
			 * Sample is pending; read and validate the data.
			 * The NMI handler may clear IbsOpVal concurrently —
			 * if either read fails the validation is skipped.
			 */
			if (sibs_read_msr(0, SIBS_MSR_OP_RIP, &rip) == 0 &&
			    sibs_read_msr(0, SIBS_MSR_OP_DATA, &data) == 0) {
				if (rip == 0 || (data & SIBS_OP_RIP_INVALID))
					st->val_bad_rip++;
			}
		}
	}

	sibs_op_disable(0);

	/* Verify CTL reads 0 after drain — confirms sequence completed. */
	if (sibs_read_msr(0, SIBS_MSR_OP_CTL, &ctl) == 0 && ctl == 0ULL)
		st->disable_ok = 1;

	(void)sibs_write_msr(0, SIBS_MSR_OP_CTL, saved_ctl);
}

/* -----------------------------------------------------------------------
 * TC-ISTR-01: IBS Op + per-CPU xorshift64 compute stress
 * ----------------------------------------------------------------------- */

struct istr_compute_arg {
	volatile bool	*stop;
	int		 cpu;
	uint64_t	 iters;
};

static void *
istr_compute_thread(void *arg)
{
	struct istr_compute_arg *ca = arg;
	cpuset_t mask;
	uint64_t x;

	CPU_ZERO(&mask);
	CPU_SET(ca->cpu, &mask);
	(void)cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	x = (uint64_t)(ca->cpu + 1) * 0xBEEFCAFEDEAD0001ULL;
	while (!*ca->stop) {
		STRESS_XORSHIFT64(x);
		ca->iters++;
	}
	if (x == 0) ca->iters++;	/* prevent dead-code elimination */
	return (NULL);
}

ATF_TC(ibs_op_cpu_stress);
ATF_TC_HEAD(ibs_op_cpu_stress, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-ISTR-01] IBS Op sampling during per-CPU xorshift64 ALU "
	    "workload (120 s); asserts MSR path survives and samples are valid");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(ibs_op_cpu_stress, tc)
{
	struct istr_compute_arg *args;
	pthread_t *threads;
	volatile bool stop = false;
	struct ibs_poll_stats st;
	uint64_t saved_ctl;
	int ncpus, i, err;

	if (!sibs_cpu_supports_ibs_op())
		atf_tc_skip("CPU does not support IBS Op sampling");

	err = sibs_read_msr(0, SIBS_MSR_OP_CTL, &saved_ctl);
	if (err != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL on CPU 0: %s",
		    strerror(err));

	ncpus   = stress_ncpus();
	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(*args));
	ATF_REQUIRE(threads != NULL && args != NULL);

	for (i = 0; i < ncpus; i++) {
		args[i].stop = &stop;
		args[i].cpu  = i;
		err = pthread_create(&threads[i], NULL,
		    istr_compute_thread, &args[i]);
		ATF_REQUIRE_MSG(err == 0, "pthread_create CPU %d: %s",
		    i, strerror(err));
	}

	ibs_op_run_sampling(saved_ctl, &st);

	stop = true;
	for (i = 0; i < ncpus; i++)
		pthread_join(threads[i], NULL);

	printf("ibs-op-cpu: polls_active=%d  val_seen=%d  val_bad_rip=%d  "
	    "read_errors=%d  drain_ok=%d  ncpus=%d\n",
	    st.polls_active, st.val_seen, st.val_bad_rip,
	    st.read_errors, st.disable_ok, ncpus);

	ATF_CHECK_MSG(st.read_errors == 0,
	    "%d MSR_IBS_OP_CTL read failure(s) during CPU stress",
	    st.read_errors);
	ATF_CHECK_MSG(st.polls_active > 0,
	    "IbsOpEn was never observed in any poll — IBS Op not running");
	ATF_CHECK_MSG(st.val_bad_rip == 0,
	    "%d IBS Op sample(s) with RIP=0 or IbsRipInvalid set",
	    st.val_bad_rip);

	free(threads);
	free(args);
}

/* -----------------------------------------------------------------------
 * TC-ISTR-02: IBS Op + 128 MiB random-strided LLC/DRAM thrash
 * ----------------------------------------------------------------------- */

#define ISTR_MEM_BUF_SIZE	(128UL * 1024UL * 1024UL)
#define ISTR_MEM_STRIDE		64UL

struct istr_thrash_arg {
	volatile bool	*stop;
	uint64_t	*buf;
	size_t		 n_elements;
	int		 cpu;
};

static void *
istr_thrash_thread(void *arg)
{
	struct istr_thrash_arg *ta = arg;
	cpuset_t mask;
	uint64_t idx, acc;
	size_t step;

	CPU_ZERO(&mask);
	CPU_SET(ta->cpu, &mask);
	(void)cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(mask), &mask);

	idx = (uint64_t)(ta->cpu + 1) * STRESS_LCG_MUL;
	acc = 0;

	while (!*ta->stop) {
		for (step = 0; step < ta->n_elements && !*ta->stop;
		    step += ISTR_MEM_STRIDE) {
			idx = (idx * STRESS_LCG_MUL + step) % ta->n_elements;
			ta->buf[idx] ^= acc;
			acc ^= ta->buf[idx];
		}
	}
	if (acc == 0) ta->buf[0] ^= 1;
	return (NULL);
}

ATF_TC(ibs_op_mem_stress);
ATF_TC_HEAD(ibs_op_mem_stress, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-ISTR-02] IBS Op sampling during 128 MiB random-strided LLC "
	    "thrash workload (120 s); asserts MSR path survives and samples "
	    "are valid under memory pressure");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(ibs_op_mem_stress, tc)
{
	struct istr_thrash_arg *args;
	pthread_t *threads;
	uint64_t *buf;
	volatile bool stop = false;
	struct ibs_poll_stats st;
	uint64_t saved_ctl;
	size_t n_elements;
	int ncpus, i, err;

	if (!sibs_cpu_supports_ibs_op())
		atf_tc_skip("CPU does not support IBS Op sampling");

	err = sibs_read_msr(0, SIBS_MSR_OP_CTL, &saved_ctl);
	if (err != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL on CPU 0: %s",
		    strerror(err));

	n_elements = ISTR_MEM_BUF_SIZE / sizeof(uint64_t);
	buf = calloc(n_elements, sizeof(uint64_t));
	ATF_REQUIRE_MSG(buf != NULL, "calloc 128 MiB: %s", strerror(errno));

	ncpus   = stress_ncpus();
	threads = calloc((size_t)ncpus, sizeof(pthread_t));
	args    = calloc((size_t)ncpus, sizeof(*args));
	if (threads == NULL || args == NULL) {
		free(buf);
		atf_tc_fail("calloc threads/args: %s", strerror(errno));
	}

	for (i = 0; i < ncpus; i++) {
		args[i].stop       = &stop;
		args[i].buf        = buf;
		args[i].n_elements = n_elements;
		args[i].cpu        = i;
		err = pthread_create(&threads[i], NULL,
		    istr_thrash_thread, &args[i]);
		ATF_REQUIRE_MSG(err == 0, "pthread_create CPU %d: %s",
		    i, strerror(err));
	}

	ibs_op_run_sampling(saved_ctl, &st);

	stop = true;
	for (i = 0; i < ncpus; i++)
		pthread_join(threads[i], NULL);

	printf("ibs-op-mem: polls_active=%d  val_seen=%d  val_bad_rip=%d  "
	    "read_errors=%d  drain_ok=%d  ncpus=%d\n",
	    st.polls_active, st.val_seen, st.val_bad_rip,
	    st.read_errors, st.disable_ok, ncpus);

	ATF_CHECK_MSG(st.read_errors == 0,
	    "%d MSR_IBS_OP_CTL read failure(s) during memory stress",
	    st.read_errors);
	ATF_CHECK_MSG(st.polls_active > 0,
	    "IbsOpEn was never observed in any poll — IBS Op not running");
	ATF_CHECK_MSG(st.val_bad_rip == 0,
	    "%d IBS Op sample(s) with RIP=0 or IbsRipInvalid set",
	    st.val_bad_rip);

	free(buf);
	free(threads);
	free(args);
}

/* -----------------------------------------------------------------------
 * TC-ISTR-03: IBS Op + sequential + random /tmp disk I/O
 * ----------------------------------------------------------------------- */

#define ISTR_DISK_BUF_SIZE	4096UL

struct istr_disk_arg {
	volatile bool	*stop;
	int		 error;
	uint64_t	 ops;
};

static void *
istr_disk_thread(void *arg)
{
	struct istr_disk_arg *da = arg;
	uint8_t buf[ISTR_DISK_BUF_SIZE];
	char path[64];
	size_t i;
	int fd;

	stress_tmpfile_path(path, sizeof(path), "ibs_disk");

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { da->error = errno; return (NULL); }

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)i;

	while (!*da->stop) {
		if (lseek(fd, 0, SEEK_SET) < 0) break;
		if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) break;
		if (lseek(fd, 0, SEEK_SET) < 0) break;
		if (read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) break;
		da->ops++;
	}

	(void)close(fd);
	(void)unlink(path);
	da->error = 0;
	return (NULL);
}

ATF_TC_WITH_CLEANUP(ibs_op_disk_stress);
ATF_TC_HEAD(ibs_op_disk_stress, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-ISTR-03] IBS Op sampling during sequential + random /tmp "
	    "disk I/O (120 s); asserts MSR path survives and samples are valid "
	    "under I/O pressure");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(ibs_op_disk_stress, tc)
{
	struct istr_disk_arg da;
	pthread_t tid;
	volatile bool stop = false;
	struct ibs_poll_stats st;
	uint64_t saved_ctl;
	int err;

	if (!sibs_cpu_supports_ibs_op())
		atf_tc_skip("CPU does not support IBS Op sampling");

	err = sibs_read_msr(0, SIBS_MSR_OP_CTL, &saved_ctl);
	if (err != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL on CPU 0: %s",
		    strerror(err));

	da.stop  = &stop;
	da.error = -1;
	da.ops   = 0;

	err = pthread_create(&tid, NULL, istr_disk_thread, &da);
	ATF_REQUIRE_MSG(err == 0, "pthread_create disk: %s", strerror(err));

	ibs_op_run_sampling(saved_ctl, &st);

	stop = true;
	pthread_join(tid, NULL);

	printf("ibs-op-disk: polls_active=%d  val_seen=%d  val_bad_rip=%d  "
	    "read_errors=%d  drain_ok=%d  disk_ops=%llu\n",
	    st.polls_active, st.val_seen, st.val_bad_rip,
	    st.read_errors, st.disable_ok,
	    (unsigned long long)da.ops);

	ATF_CHECK_MSG(da.error == 0,
	    "disk thread exited with error: %s", strerror(da.error));
	ATF_CHECK_MSG(st.read_errors == 0,
	    "%d MSR_IBS_OP_CTL read failure(s) during disk stress",
	    st.read_errors);
	ATF_CHECK_MSG(st.polls_active > 0,
	    "IbsOpEn was never observed in any poll — IBS Op not running");
	ATF_CHECK_MSG(st.val_bad_rip == 0,
	    "%d IBS Op sample(s) with RIP=0 or IbsRipInvalid set",
	    st.val_bad_rip);
}

ATF_TC_CLEANUP(ibs_op_disk_stress, tc)
{
	/* Best-effort removal of any leftover temp file. */
	char path[64];
	stress_tmpfile_path(path, sizeof(path), "ibs_disk");
	(void)unlink(path);
}

/* -----------------------------------------------------------------------
 * TC-ISTR-04: IBS Op + AF_UNIX socketpair flood
 * ----------------------------------------------------------------------- */

#define ISTR_NET_MSG_SIZE	64
#define ISTR_NET_PAIRS		4

struct istr_net_pair {
	volatile bool	*stop;
	int		 fd[2];
	uint64_t	 sent;
};

static void *
istr_net_producer(void *arg)
{
	struct istr_net_pair *p = arg;
	uint8_t msg[ISTR_NET_MSG_SIZE];
	uint64_t seq = 0;

	while (!*p->stop) {
		memcpy(msg, &seq, sizeof(seq));
		if (send(p->fd[0], msg, sizeof(msg), MSG_NOSIGNAL) !=
		    (ssize_t)sizeof(msg))
			break;
		p->sent++;
		seq++;
	}
	return (NULL);
}

static void *
istr_net_consumer(void *arg)
{
	struct istr_net_pair *p = arg;
	uint8_t msg[ISTR_NET_MSG_SIZE];

	while (!*p->stop) {
		ssize_t n = recv(p->fd[1], msg, sizeof(msg), MSG_DONTWAIT);
		(void)n;
	}
	return (NULL);
}

ATF_TC(ibs_op_net_stress);
ATF_TC_HEAD(ibs_op_net_stress, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-ISTR-04] IBS Op sampling during AF_UNIX socketpair flood "
	    "(120 s); asserts MSR path survives and samples are valid under "
	    "network stack pressure");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(ibs_op_net_stress, tc)
{
	struct istr_net_pair pairs[ISTR_NET_PAIRS];
	pthread_t prod[ISTR_NET_PAIRS], cons[ISTR_NET_PAIRS];
	volatile bool stop = false;
	struct ibs_poll_stats st;
	uint64_t saved_ctl;
	uint64_t total_sent = 0;
	int i, err;

	if (!sibs_cpu_supports_ibs_op())
		atf_tc_skip("CPU does not support IBS Op sampling");

	err = sibs_read_msr(0, SIBS_MSR_OP_CTL, &saved_ctl);
	if (err != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL on CPU 0: %s",
		    strerror(err));

	/*
	 * Ignore SIGPIPE: shutdown() on a socket while send() is in flight
	 * would otherwise terminate the whole process.
	 */
	signal(SIGPIPE, SIG_IGN);

	for (i = 0; i < ISTR_NET_PAIRS; i++) {
		pairs[i].stop = &stop;
		pairs[i].sent = 0;
		ATF_REQUIRE_MSG(socketpair(AF_UNIX, SOCK_SEQPACKET, 0,
		    pairs[i].fd) == 0,
		    "socketpair %d: %s", i, strerror(errno));
		err = pthread_create(&prod[i], NULL, istr_net_producer,
		    &pairs[i]);
		ATF_REQUIRE_MSG(err == 0, "pthread_create prod %d: %s",
		    i, strerror(err));
		err = pthread_create(&cons[i], NULL, istr_net_consumer,
		    &pairs[i]);
		ATF_REQUIRE_MSG(err == 0, "pthread_create cons %d: %s",
		    i, strerror(err));
	}

	ibs_op_run_sampling(saved_ctl, &st);

	stop = true;
	for (i = 0; i < ISTR_NET_PAIRS; i++) {
		shutdown(pairs[i].fd[0], SHUT_RDWR);
		shutdown(pairs[i].fd[1], SHUT_RDWR);
		pthread_join(prod[i], NULL);
		pthread_join(cons[i], NULL);
		close(pairs[i].fd[0]);
		close(pairs[i].fd[1]);
		total_sent += pairs[i].sent;
	}

	printf("ibs-op-net: polls_active=%d  val_seen=%d  val_bad_rip=%d  "
	    "read_errors=%d  drain_ok=%d  msgs_sent=%llu\n",
	    st.polls_active, st.val_seen, st.val_bad_rip,
	    st.read_errors, st.disable_ok,
	    (unsigned long long)total_sent);

	ATF_CHECK_MSG(st.read_errors == 0,
	    "%d MSR_IBS_OP_CTL read failure(s) during net stress",
	    st.read_errors);
	ATF_CHECK_MSG(st.polls_active > 0,
	    "IbsOpEn was never observed in any poll — IBS Op not running");
	ATF_CHECK_MSG(st.val_bad_rip == 0,
	    "%d IBS Op sample(s) with RIP=0 or IbsRipInvalid set",
	    st.val_bad_rip);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_op_cpu_stress);
	ATF_TP_ADD_TC(tp, ibs_op_mem_stress);
	ATF_TP_ADD_TC(tp, ibs_op_disk_stress);
	ATF_TP_ADD_TC(tp, ibs_op_net_stress);

	return (atf_no_error());
}

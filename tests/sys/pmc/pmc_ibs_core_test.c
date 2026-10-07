/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other advertising materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Regression test for core counter sampling next to IBS sampling on AMD.
 *
 * IBS and the core counters share the NMI.  amd_intr() and amd_intr_v2()
 * used to return as soon as the IBS handler claimed an NMI, so a core
 * counter overflow delivered in the same NMI was never serviced: the
 * counter was not reloaded and sampling with it stopped.  The case samples
 * unhalted cycles on a busy CPU 0 first alone and then together with IBS
 * op, and requires the second phase to collect at least half as many core
 * samples as the first.  On an EPYC 9654 the broken handler collects at most
 * a few hundred core samples in the second phase instead of about 7300.
 *
 * The case uses system-mode PMCs on CPU 0, so it needs root and an
 * exclusive run, and it is skipped where hwpmc or IBS is not available.
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <pmc.h>
#include <pmclog.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atf-c.h>

#define	PHASE_SECONDS	2
#define	CORE_RATE	1000000
#define	IBS_RATE	65536

#define	LOG_FILE	"pmc.log"

static const char *const core_events[] = {
	"unhalted-cycles", "unhalted-core-cycles", "ls_not_halted_cyc", NULL
};

static pmc_id_t
alloc_core(void)
{
	pmc_id_t id;
	int k;

	for (k = 0; core_events[k] != NULL; k++) {
		if (pmc_allocate(core_events[k], PMC_MODE_SS, 0, 0, &id,
		    CORE_RATE) == 0)
			return (id);
	}
	atf_tc_skip("no core cycle counter can sample on CPU 0: %s",
	    strerror(errno));
}

/* Keep CPU 0 busy, so that the cycle counter keeps counting. */
static void
spin(void)
{
	struct timespec end, now;

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	end.tv_sec += PHASE_SECONDS;
	do {
		ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	} while (now.tv_sec < end.tv_sec ||
	    (now.tv_sec == end.tv_sec && now.tv_nsec < end.tv_nsec));
}

/*
 * pmc_close_logfile() only starts the close, and the log thread writes the
 * rest of the log afterwards: wait until the file stops growing.
 */
static void
wait_log_written(int fd)
{
	struct stat st;
	off_t last;
	int i, stable;

	last = -1;
	stable = 0;
	for (i = 0; i < 100 && stable < 5; i++) {
		usleep(100000);
		ATF_REQUIRE(fstat(fd, &st) == 0);
		if (st.st_size == last)
			stable++;
		else
			stable = 0;
		last = st.st_size;
	}
	ATF_REQUIRE_MSG(stable == 5, "the log was still growing after 10 s");
}

/* Count the samples logged for PMCs a and b. */
static void
count_samples(int fd, pmc_id_t a, pmc_id_t b, uintmax_t *na, uintmax_t *nb)
{
	struct pmclog_ev ev;
	void *log;

	*na = *nb = 0;
	ATF_REQUIRE(lseek(fd, 0, SEEK_SET) == 0);
	log = pmclog_open(fd);
	ATF_REQUIRE_MSG(log != NULL, "pmclog_open: %s", strerror(errno));
	while (pmclog_read(log, &ev) == 0) {
		if (ev.pl_type != PMCLOG_TYPE_CALLCHAIN)
			continue;
		if (ev.pl_u.pl_cc.pl_pmcid == a)
			(*na)++;
		else if (ev.pl_u.pl_cc.pl_pmcid == b)
			(*nb)++;
	}
	ATF_CHECK_MSG(ev.pl_state == PMCLOG_EOF,
	    "the log did not parse to its end (state %d)", ev.pl_state);
	pmclog_close(log);
}

ATF_TC(core_sampling_with_ibs);
ATF_TC_HEAD(core_sampling_with_ibs, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A core counter keeps sampling while IBS op samples the same CPU");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(core_sampling_with_ibs, tc)
{
	cpuset_t set;
	pmc_id_t alone, ibs, with_ibs;
	uintmax_t na, nb;
	int fd;

	if (pmc_init() != 0)
		atf_tc_skip("hwpmc(4) is not available");
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	ATF_REQUIRE(cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(set), &set) == 0);

	fd = open(LOG_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", LOG_FILE, strerror(errno));
	ATF_REQUIRE_MSG(pmc_configure_logfile(fd) == 0,
	    "pmc_configure_logfile: %s", strerror(errno));

	if (pmc_allocate("ibs-op", PMC_MODE_SS, 0, 0, &ibs, IBS_RATE) != 0)
		atf_tc_skip("no IBS op PMC on CPU 0: %s", strerror(errno));
	/* Both core PMCs exist at once, so their ids differ. */
	alone = alloc_core();
	with_ibs = alloc_core();

	ATF_REQUIRE(pmc_start(alone) == 0);
	spin();
	ATF_REQUIRE(pmc_stop(alone) == 0);

	ATF_REQUIRE(pmc_start(with_ibs) == 0);
	ATF_REQUIRE(pmc_start(ibs) == 0);
	spin();
	ATF_REQUIRE(pmc_stop(ibs) == 0);
	ATF_REQUIRE(pmc_stop(with_ibs) == 0);

	ATF_REQUIRE(pmc_flush_logfile() == 0);
	ATF_REQUIRE(pmc_release(ibs) == 0);
	ATF_REQUIRE(pmc_release(with_ibs) == 0);
	ATF_REQUIRE(pmc_release(alone) == 0);
	ATF_REQUIRE(pmc_close_logfile() == 0);

	wait_log_written(fd);
	count_samples(fd, alone, with_ibs, &na, &nb);
	(void)close(fd);

	ATF_REQUIRE_MSG(na > 0, "the core counter took no samples on its own");
	ATF_CHECK_MSG(nb * 2 >= na,
	    "the core counter took %ju samples on its own but only %ju with "
	    "IBS op sampling the same CPU", na, nb);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, core_sampling_with_ibs);

	return (atf_no_error());
}

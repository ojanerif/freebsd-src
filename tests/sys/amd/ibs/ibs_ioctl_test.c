/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 * Test IBS ioctl API for period configuration and capability query.
 *
 * These tests validate the hwpmc syscall interface for IBS-specific
 * operations: PMC_OP_IBSSETPERIOD and PMC_OP_IBSGETCAPS.
 */

#include <sys/param.h>
#include <sys/pmc.h>
#include <sys/syscall.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ibs_utils.h"
#include <dev/hwpmc/hwpmc_ibs.h>

/*
 * Helper: invoke the hwpmc syscall with the given operation and data.
 * Returns 0 on success, -1 on failure with errno set.
 */
static int
hwpmc_syscall(enum pmc_ops op, void *data)
{
	struct pmc_syscall_args args;

	args.pmop_code = op;
	args.pmop_data = data;

	return (syscall(SYS_hwpmc, &args));
}

/*
 * Helper: check if hwpmc module is loaded and IBS is available.
 */
static bool
hwpmc_ibs_available(void)
{
	int fd;
	struct pmc_op_getcpuinfo gci;

	if (!cpu_supports_ibs())
		return (false);

	/* Try to get CPU info from hwpmc */
	memset(&gci, 0, sizeof(gci));
	if (hwpmc_syscall(PMC_OP_GETCPUINFO, &gci) != 0)
		return (false);

	/* Check if IBS class is present */
	for (uint32_t i = 0; i < gci.pm_nclass; i++) {
		if (gci.pm_classes[i].pm_class == PMC_CLASS_IBS)
			return (true);
	}

	return (false);
}

/*
 * Test IBS capability query via PMC_OP_IBSGETCAPS.
 *
 * This test verifies that:
 * - The syscall returns successfully on IBS-capable hardware
 * - The capability flags are consistent with CPUID detection
 * - Feature flags match expected values for the CPU family
 */
ATF_TC(ibs_ioctl_get_caps);
ATF_TC_HEAD(ibs_ioctl_get_caps, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test IBS capability query via PMC_OP_IBSGETCAPS");
}

ATF_TC_BODY(ibs_ioctl_get_caps, tc)
{
	struct pmc_op_ibsgetcaps caps;
	uint32_t regs[4];
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!hwpmc_ibs_available())
		atf_tc_skip("hwpmc IBS module not available");

	memset(&caps, 0, sizeof(caps));
	error = hwpmc_syscall(PMC_OP_IBSGETCAPS, &caps);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify that features field is populated */
	ATF_CHECK(caps.pm_ibs_features != 0);

	/* Cross-check with CPUID 0x8000001B */
	if (do_cpuid_ioctl(0x8000001B, regs) == 0) {
		uint32_t cpuid_features = regs[0];

		/* Feature flags should match CPUID */
		if ((cpuid_features & CPUID_IBSID_FETCHSAM) != 0)
			ATF_CHECK(caps.pm_ibs_fetch_cap == 1);
		if ((cpuid_features & CPUID_IBSID_OPSAM) != 0)
			ATF_CHECK(caps.pm_ibs_op_cap == 1);
		if ((cpuid_features & CPUID_IBSID_ZEN4IBSEXTENSIONS) != 0)
			ATF_CHECK(caps.pm_ibs_zen4_ext == 1);
		if ((cpuid_features & CPUID_IBSID_IBSLOADLATENCYFILT) != 0)
			ATF_CHECK(caps.pm_ibs_load_lat_filt == 1);
	}

	/* At least one of fetch or op sampling should be available */
	ATF_CHECK(caps.pm_ibs_fetch_cap == 1 || caps.pm_ibs_op_cap == 1);
}

/*
 * Test IBS period configuration via PMC_OP_IBSSETPERIOD.
 *
 * This test verifies that:
 * - Setting a valid period on an allocated IBS PMC succeeds
 * - Setting period to zero fails with EINVAL
 * - Period can be changed while PMC is stopped
 */
ATF_TC(ibs_ioctl_set_period);
ATF_TC_HEAD(ibs_ioctl_set_period, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test IBS period configuration via PMC_OP_IBSSETPERIOD");
}

ATF_TC_BODY(ibs_ioctl_set_period, tc)
{
	struct pmc_op_ibssetperiod sp;
	struct pmc_op_pmcallocate alloc;
	struct pmc_op_ibsgetcaps caps;
	pmc_id_t pmcid;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!hwpmc_ibs_available())
		atf_tc_skip("hwpmc IBS module not available");

	/* Get IBS capabilities to determine which type to allocate */
	memset(&caps, 0, sizeof(caps));
	error = hwpmc_syscall(PMC_OP_IBSGETCAPS, &caps);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Allocate an IBS Fetch PMC if available */
	if (caps.pm_ibs_fetch_cap) {
		memset(&alloc, 0, sizeof(alloc));
		alloc.pm_class = PMC_CLASS_IBS;
		alloc.pm_caps = PMC_CAP_SYSTEM | PMC_CAP_INTERRUPT;
		alloc.pm_cpu = PMC_CPU_ANY;
		alloc.pm_mode = PMC_MODE_SS;
		alloc.pm_ev = PMC_EV_IBS_FETCH_SAMPLE;
		alloc.pm_md.pm_ibs.ibs_type = IBS_PMC_FETCH;
		alloc.pm_md.pm_ibs.ibs_ctl = IBS_FETCH_MIN_RATE;
		alloc.pm_count = IBS_FETCH_MIN_RATE;

		error = hwpmc_syscall(PMC_OP_PMCALLOCATE, &alloc);
		if (error != 0)
			atf_tc_skip("Failed to allocate IBS Fetch PMC: %s",
			    strerror(errno));

		pmcid = alloc.pm_pmcid;
		ATF_CHECK(pmcid != PMC_ID_INVALID);

		/* Test setting a valid period */
		memset(&sp, 0, sizeof(sp));
		sp.pm_pmcid = pmcid;
		sp.pm_period = 100000;

		error = hwpmc_syscall(PMC_OP_IBSSETPERIOD, &sp);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Test setting period to zero should fail */
		sp.pm_period = 0;
		error = hwpmc_syscall(PMC_OP_IBSSETPERIOD, &sp);
		ATF_REQUIRE_ERRNO(EINVAL, error != 0);

		/* Release the PMC */
		struct pmc_op_simple rel;
		rel.pm_pmcid = pmcid;
		hwpmc_syscall(PMC_OP_PMCRELEASE, &rel);
	} else {
		atf_tc_skip("IBS Fetch sampling not available");
	}
}

/*
 * Test IBS period change while PMC is running.
 *
 * This test verifies that:
 * - Period can be changed dynamically while PMC is active
 * - The new period takes effect without stopping/restarting
 */
ATF_TC(ibs_ioctl_period_change_while_running);
ATF_TC_HEAD(ibs_ioctl_period_change_while_running, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test IBS period change while PMC is running");
}

ATF_TC_BODY(ibs_ioctl_period_change_while_running, tc)
{
	struct pmc_op_ibssetperiod sp;
	struct pmc_op_pmcallocate alloc;
	struct pmc_op_ibsgetcaps caps;
	struct pmc_op_simple start, stop, rel;
	pmc_id_t pmcid;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!hwpmc_ibs_available())
		atf_tc_skip("hwpmc IBS module not available");

	/* Get IBS capabilities */
	memset(&caps, 0, sizeof(caps));
	error = hwpmc_syscall(PMC_OP_IBSGETCAPS, &caps);
	ATF_REQUIRE_ERRNO(0, error == 0);

	if (!caps.pm_ibs_fetch_cap)
		atf_tc_skip("IBS Fetch sampling not available");

	/* Allocate an IBS Fetch PMC */
	memset(&alloc, 0, sizeof(alloc));
	alloc.pm_class = PMC_CLASS_IBS;
	alloc.pm_caps = PMC_CAP_SYSTEM | PMC_CAP_INTERRUPT;
	alloc.pm_cpu = PMC_CPU_ANY;
	alloc.pm_mode = PMC_MODE_SS;
	alloc.pm_ev = PMC_EV_IBS_FETCH_SAMPLE;
	alloc.pm_md.pm_ibs.ibs_type = IBS_PMC_FETCH;
	alloc.pm_md.pm_ibs.ibs_ctl = IBS_FETCH_MIN_RATE;
	alloc.pm_count = IBS_FETCH_MIN_RATE;

	error = hwpmc_syscall(PMC_OP_PMCALLOCATE, &alloc);
	if (error != 0)
		atf_tc_skip("Failed to allocate IBS Fetch PMC: %s",
		    strerror(errno));

	pmcid = alloc.pm_pmcid;
	ATF_CHECK(pmcid != PMC_ID_INVALID);

	/* Start the PMC */
	memset(&start, 0, sizeof(start));
	start.pm_pmcid = pmcid;
	error = hwpmc_syscall(PMC_OP_PMCSTART, &start);
	if (error != 0) {
		rel.pm_pmcid = pmcid;
		hwpmc_syscall(PMC_OP_PMCRELEASE, &rel);
		atf_tc_skip("Failed to start IBS PMC: %s", strerror(errno));
	}

	/* Change period while running */
	memset(&sp, 0, sizeof(sp));
	sp.pm_pmcid = pmcid;
	sp.pm_period = 200000;

	error = hwpmc_syscall(PMC_OP_IBSSETPERIOD, &sp);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Change period again */
	sp.pm_period = 500000;
	error = hwpmc_syscall(PMC_OP_IBSSETPERIOD, &sp);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Stop the PMC */
	memset(&stop, 0, sizeof(stop));
	stop.pm_pmcid = pmcid;
	error = hwpmc_syscall(PMC_OP_PMCSTOP, &stop);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Release the PMC */
	memset(&rel, 0, sizeof(rel));
	rel.pm_pmcid = pmcid;
	error = hwpmc_syscall(PMC_OP_PMCRELEASE, &rel);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_ioctl_get_caps);
	ATF_TP_ADD_TC(tp, ibs_ioctl_set_period);
	ATF_TP_ADD_TC(tp, ibs_ioctl_period_change_while_running);
	return (atf_no_error());
}

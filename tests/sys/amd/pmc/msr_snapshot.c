/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   Read AMD Zen core PMC MSRs through cpuctl(4) for ATF characterization.
 */

#include <sys/types.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../amd_pmc_test_common.h"
#include "msr_snapshot.h"

#define	AMD_CPUID_EXTPERFMON	0x80000022U
#define	AMD_PERFMON_V2_PRESENT	0x00000001U

static void
msr_snapshot_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (errbuf == NULL || errlen == 0)
		return;
	va_start(ap, fmt);
	(void)vsnprintf(errbuf, errlen, fmt, ap);
	va_end(ap);
}

static int
amd_msr_read_cpu(int cpu, uint32_t msr, uint64_t *value, char *errbuf,
    size_t errlen)
{
	cpuctl_msr_args_t args;
	char path[64];
	int error, fd;

	(void)snprintf(path, sizeof(path), "/dev/cpuctl%d", cpu);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		error = errno != 0 ? errno : ENOENT;
		msr_snapshot_err(errbuf, errlen, "open(%s) failed: %s", path,
		    strerror(error));
		return (error);
	}

	memset(&args, 0, sizeof(args));
	args.msr = (int)msr;
	if (ioctl(fd, CPUCTL_RDMSR, &args) < 0) {
		error = errno != 0 ? errno : EINVAL;
		(void)close(fd);
		msr_snapshot_err(errbuf, errlen,
		    "CPUCTL_RDMSR cpu=%d msr=0x%x failed: %s", cpu, msr,
		    strerror(error));
		return (error);
	}

	(void)close(fd);
	*value = args.data;
	return (0);
}

int
amd_msr_detect_perfmon_v2(bool *present, unsigned int *num_core_pmcs,
    char *errbuf, size_t errlen)
{
	uint32_t regs[4];
	unsigned int reported;
	int error;

	/*
	 * Default to the six legacy programmable core PMC pairs shared by every
	 * AMD Zen generation.  Zen 1, Zen+, Zen 2 and Zen 3 do not implement
	 * PerfMonV2; reading MSRC001_020C..MSRC001_021F on those parts via
	 * cpuctl(4) raises #GP and aborts the snapshot.  Only override this
	 * default once CPUID Fn80000022 EAX[0] confirms PerfMonV2 is present.
	 */
	*present = false;
	*num_core_pmcs = AMD_MSR_LEGACY_CORE_PMCS;

	error = amd_test_do_cpuid(AMD_CPUID_EXT_BASE, regs);
	if (error != 0) {
		msr_snapshot_err(errbuf, errlen,
		    "CPUID 0x%x failed: %s", AMD_CPUID_EXT_BASE,
		    strerror(error));
		return (error);
	}
	if (regs[0] < AMD_CPUID_EXTPERFMON)
		return (0);

	error = amd_test_do_cpuid(AMD_CPUID_EXTPERFMON, regs);
	if (error != 0) {
		msr_snapshot_err(errbuf, errlen,
		    "CPUID 0x%x failed: %s", AMD_CPUID_EXTPERFMON,
		    strerror(error));
		return (error);
	}

	if ((regs[0] & AMD_PERFMON_V2_PRESENT) == 0)
		return (0);

	*present = true;
	reported = regs[1] & 0x0fU;
	if (reported < AMD_MSR_LEGACY_CORE_PMCS)
		reported = AMD_MSR_LEGACY_CORE_PMCS;
	if (reported > AMD_MSR_SNAPSHOT_CORE_LIMIT)
		reported = AMD_MSR_SNAPSHOT_CORE_LIMIT;
	*num_core_pmcs = reported;
	return (0);
}

int
amd_msr_snapshot_take(int cpu, struct amd_msr_snapshot *snap, char *errbuf,
    size_t errlen)
{
	uint32_t msr;
	unsigned int i;
	int error;

	memset(snap, 0, sizeof(*snap));
	snap->cpu = cpu;
	error = amd_msr_detect_perfmon_v2(&snap->perfmon_v2,
	    &snap->num_core_pmcs, errbuf, errlen);
	if (error != 0)
		return (error);

	for (i = 0; i < snap->num_core_pmcs; i++) {
		msr = AMD_MSR_CORE_BASE + i * AMD_MSR_CORE_STRIDE;
		error = amd_msr_read_cpu(cpu, msr, &snap->evtsel[i], errbuf,
		    errlen);
		if (error != 0)
			return (error);
		error = amd_msr_read_cpu(cpu, msr + 1, &snap->perfctr[i], errbuf,
		    errlen);
		if (error != 0)
			return (error);
	}

	if (snap->perfmon_v2) {
		error = amd_msr_read_cpu(cpu, AMD_MSR_PERFCNTR_GLOBAL_CTL,
		    &snap->global_ctl, errbuf, errlen);
		if (error != 0)
			return (error);
		snap->global_ctl_valid = true;
	}
	return (0);
}

bool
amd_msr_snapshot_evsel_enabled(const struct amd_msr_snapshot *snap,
    unsigned int row)
{

	if (row >= snap->num_core_pmcs || row >= AMD_MSR_SNAPSHOT_CORE_LIMIT)
		return (false);
	return ((snap->evtsel[row] & AMD_MSR_PERFEVTSEL_ENABLE) != 0);
}

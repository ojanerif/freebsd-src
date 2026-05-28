/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   AMD Zen core PMC MSR snapshot helper for hwpmc(4) tests.
 */

#ifndef _AMD_PMC_MSR_SNAPSHOT_H_
#define	_AMD_PMC_MSR_SNAPSHOT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	AMD_MSR_SNAPSHOT_CORE_LIMIT	16
#define	AMD_MSR_CORE_BASE		0xC0010200U
#define	AMD_MSR_CORE_STRIDE		2U
#define	AMD_MSR_PERFCNTR_GLOBAL_CTL	0xC0000301U
#define	AMD_MSR_PERFEVTSEL_ENABLE	(1ULL << 22)

struct amd_msr_snapshot {
	int		cpu;
	unsigned int	num_core_pmcs;
	bool		perfmon_v2;
	bool		global_ctl_valid;
	uint64_t	evtsel[AMD_MSR_SNAPSHOT_CORE_LIMIT];
	uint64_t	perfctr[AMD_MSR_SNAPSHOT_CORE_LIMIT];
	uint64_t	global_ctl;
};

int	amd_msr_detect_perfmon_v2(bool *present, unsigned int *num_core_pmcs,
	    char *errbuf, size_t errlen);
int	amd_msr_snapshot_take(int cpu, struct amd_msr_snapshot *snap,
	    char *errbuf, size_t errlen);
bool	amd_msr_snapshot_evsel_enabled(const struct amd_msr_snapshot *snap,
	    unsigned int row);

#endif /* _AMD_PMC_MSR_SNAPSHOT_H_ */

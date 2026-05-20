/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: Davi Chaves Azevedo
 *
 * Pure software model for validating pmcstat's AMD Zen 3 IBS errata decode
 * policy.  This header intentionally has no hardware I/O and never consults
 * the live host CPU.  A Zen 4 host can force the Zen 3 path by passing a
 * synthetic hwpmc CPUID string, which validates only the software decode path;
 * it does not reproduce the original Zen 3 hardware errata.
 */

#ifndef _IBS_ZEN3_ERRATA_DECODE_H_
#define	_IBS_ZEN3_ERRATA_DECODE_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ibs_decode.h"

struct ibs_zen3_errata_state {
	bool	zen3_b0;
};

static inline bool
ibs_zen3_errata_cpuid_is_zen3_b0(const char *cpuid)
{
	unsigned int family, model;
	int end;

	if (cpuid == NULL)
		return (false);
	end = 0;
	if (sscanf(cpuid, "AuthenticAMD-%u-%x-%*u%n", &family, &model,
	    &end) != 2 || cpuid[end] != '\0')
		return (false);

	return (family == 0x19 && model <= 0x0f);
}

static inline void
ibs_zen3_errata_update(struct ibs_zen3_errata_state *state,
    const char *cpuid)
{

	state->zen3_b0 = ibs_zen3_errata_cpuid_is_zen3_b0(cpuid);
}

/* freebsd#1238: IbsIcMiss is invalid on affected Zen 3 B0 fetch samples. */
static inline uint64_t
ibs_zen3_errata_sanitize_fetch_ctl(const struct ibs_zen3_errata_state *state,
    uint64_t fetch_ctl)
{

	if (state->zen3_b0)
		fetch_ctl &= ~IBS_IC_MISS;
	return (fetch_ctl);
}

/*
 * freebsd#1293: on affected Zen 3 B0 OP samples, L2Miss and
 * OpDcMissOpenMemReqs are invalid when DcMissNoMabAlloc or SwPf is set.
 * IBS_OP_DATA2 is also invalid in this condition, but pmcstat does not display
 * DATA2 today, so this software model only sanitizes displayed DATA3 fields.
 */
static inline uint64_t
ibs_zen3_errata_sanitize_op_data3(const struct ibs_zen3_errata_state *state,
    uint64_t data3)
{

	if (state->zen3_b0 &&
	    (data3 & (IBS_DC_MISS_NO_MAB_ALLOC | IBS_SW_PF)) != 0)
		data3 &= ~(IBS_L2_MISS | IBS_OP_DC_MISS_OPEN_MEM_REQS);
	return (data3);
}

/* freebsd#1347: pmcstat should defer L1TLB page-size decode on Zen 3 B0. */
static inline bool
ibs_zen3_errata_defer_l1tlb_page_size(
    const struct ibs_zen3_errata_state *state)
{

	return (state->zen3_b0);
}

#endif /* _IBS_ZEN3_ERRATA_DECODE_H_ */

/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: Davi Chaves Azevedo
 *
 * [TC-UNIT-ZEN3ERR] — Software-only AMD Zen 3 IBS errata decode tests.
 *
 * These tests validate the pmcstat consumer-side errata policy with synthetic
 * hwpmc CPUID strings and synthetic IBS payload words.  They are safe on Zen 4
 * because they do not read or write MSRs, load hwpmc, use firmware knobs, or
 * reproduce the original Zen 3 hardware behavior.
 */

#include <atf-c.h>

#include "ibs_zen3_errata_decode.h"

#define	ZEN3_B0_CPUID	"AuthenticAMD-25-00-1"
#define	ZEN3_B0_EDGE_CPUID	"AuthenticAMD-25-0F-1"
#define	ZEN3_VERMEER_CPUID	"AuthenticAMD-25-20-1"
#define	ZEN4_FIRST_MODEL_CPUID	"AuthenticAMD-25-10-1"
#define	ZEN4_CPUID	"AuthenticAMD-25-11-1"
#define	ZEN5_CPUID	"AuthenticAMD-26-00-0"

static uint64_t
sample_fetch_ctl(void)
{

	return (IBS_IC_MISS | IBS_L1TLB_MISS | IBS_FETCH_L2_MISS |
	    IBS_L3_MISS_ONLY);
}

static uint64_t
sample_op_data3(uint64_t trigger)
{

	return (IBS_LD_OP | IBS_DC_MISS | trigger | IBS_L2_MISS |
	    (0x2aULL << 26));
}

/* TC-UNIT-ZEN3ERR-01: affected Family 19h model 00h-0Fh is detected. */
ATF_TC_WITHOUT_HEAD(ibs_unit_zen3_errata_detects_affected_cpu);
ATF_TC_BODY(ibs_unit_zen3_errata_detects_affected_cpu, tc)
{
	struct ibs_zen3_errata_state state = { 0 };

	ibs_zen3_errata_update(&state, ZEN3_B0_CPUID);
	ATF_CHECK(state.zen3_b0);
	ATF_CHECK(ibs_zen3_errata_cpuid_is_zen3_b0(ZEN3_B0_EDGE_CPUID));
}

/* TC-UNIT-ZEN3ERR-02: affected fetch samples suppress invalid IbsIcMiss. */
ATF_TC_WITHOUT_HEAD(ibs_unit_zen3_errata_sanitizes_fetch_icmiss);
ATF_TC_BODY(ibs_unit_zen3_errata_sanitizes_fetch_icmiss, tc)
{
	struct ibs_zen3_errata_state state = { 0 };
	uint64_t ctl;

	ibs_zen3_errata_update(&state, ZEN3_B0_CPUID);
	ctl = ibs_zen3_errata_sanitize_fetch_ctl(&state, sample_fetch_ctl());

	ATF_CHECK_EQ(0, ctl & IBS_IC_MISS);
	ATF_CHECK((ctl & IBS_L1TLB_MISS) != 0);
	ATF_CHECK((ctl & IBS_FETCH_L2_MISS) != 0);
	ATF_CHECK((ctl & IBS_L3_MISS_ONLY) != 0);
}

/* TC-UNIT-ZEN3ERR-03: NoMabAlloc suppresses invalid OP DATA3 fields. */
ATF_TC_WITHOUT_HEAD(ibs_unit_zen3_errata_sanitizes_op_data3_nomab);
ATF_TC_BODY(ibs_unit_zen3_errata_sanitizes_op_data3_nomab, tc)
{
	struct ibs_zen3_errata_state state = { 0 };
	uint64_t data3;

	ibs_zen3_errata_update(&state, ZEN3_B0_CPUID);
	data3 = ibs_zen3_errata_sanitize_op_data3(&state,
	    sample_op_data3(IBS_DC_MISS_NO_MAB_ALLOC));

	ATF_CHECK_EQ(0, data3 & IBS_L2_MISS);
	ATF_CHECK_EQ(0, data3 & IBS_OP_DC_MISS_OPEN_MEM_REQS);
	ATF_CHECK((data3 & IBS_DC_MISS_NO_MAB_ALLOC) != 0);
	ATF_CHECK((data3 & IBS_LD_OP) != 0);
	ATF_CHECK((data3 & IBS_DC_MISS) != 0);
}

/* TC-UNIT-ZEN3ERR-04: SwPf suppresses invalid OP DATA3 fields. */
ATF_TC_WITHOUT_HEAD(ibs_unit_zen3_errata_sanitizes_op_data3_swpf);
ATF_TC_BODY(ibs_unit_zen3_errata_sanitizes_op_data3_swpf, tc)
{
	struct ibs_zen3_errata_state state = { 0 };
	uint64_t data3;

	ibs_zen3_errata_update(&state, ZEN3_B0_CPUID);
	data3 = ibs_zen3_errata_sanitize_op_data3(&state,
	    sample_op_data3(IBS_SW_PF));

	ATF_CHECK_EQ(0, data3 & IBS_L2_MISS);
	ATF_CHECK_EQ(0, data3 & IBS_OP_DC_MISS_OPEN_MEM_REQS);
	ATF_CHECK((data3 & IBS_SW_PF) != 0);
}

/* TC-UNIT-ZEN3ERR-05: Zen 4 synthetic CPUID is unaffected. */
ATF_TC_WITHOUT_HEAD(ibs_unit_zen4_unaffected_not_sanitized);
ATF_TC_BODY(ibs_unit_zen4_unaffected_not_sanitized, tc)
{
	struct ibs_zen3_errata_state state = { 0 };
	uint64_t fetch_ctl, data3;

	ibs_zen3_errata_update(&state, ZEN4_CPUID);
	ATF_REQUIRE(!state.zen3_b0);

	fetch_ctl = ibs_zen3_errata_sanitize_fetch_ctl(&state,
	    sample_fetch_ctl());
	data3 = ibs_zen3_errata_sanitize_op_data3(&state,
	    sample_op_data3(IBS_SW_PF));

	ATF_CHECK_EQ(sample_fetch_ctl(), fetch_ctl);
	ATF_CHECK_EQ(sample_op_data3(IBS_SW_PF), data3);
	ATF_CHECK(!ibs_zen3_errata_defer_l1tlb_page_size(&state));
}

/* TC-UNIT-ZEN3ERR-06: forced Zen 3 path is synthetic software only. */
ATF_TC_WITHOUT_HEAD(ibs_unit_forced_zen3_path_is_software_only);
ATF_TC_BODY(ibs_unit_forced_zen3_path_is_software_only, tc)
{
	struct ibs_zen3_errata_state state = { 0 };
	uint64_t data3;

	/* Synthetic CPUID forces Zen 3 decode; live host generation is ignored. */
	ibs_zen3_errata_update(&state, ZEN3_B0_CPUID);
	data3 = ibs_zen3_errata_sanitize_op_data3(&state,
	    sample_op_data3(IBS_SW_PF));

	ATF_CHECK(state.zen3_b0);
	ATF_CHECK_EQ(0, data3 & IBS_L2_MISS);
	ATF_CHECK_EQ(0, data3 & IBS_OP_DC_MISS_OPEN_MEM_REQS);
	ATF_CHECK(ibs_zen3_errata_defer_l1tlb_page_size(&state));
}

/* TC-UNIT-ZEN3ERR-07: default state and invalid CPUID strings are disabled. */
ATF_TC_WITHOUT_HEAD(ibs_unit_default_disabled_and_invalid_inputs);
ATF_TC_BODY(ibs_unit_default_disabled_and_invalid_inputs, tc)
{
	struct ibs_zen3_errata_state state = { 0 };
	uint64_t data3;

	data3 = sample_op_data3(IBS_SW_PF);
	ATF_CHECK(!state.zen3_b0);
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0(NULL));
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0("GenuineIntel-6-8F-8"));
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0("AuthenticAMD-bad"));
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0("AuthenticAMD-25-00"));
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0(
	    ZEN4_FIRST_MODEL_CPUID));
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0(ZEN3_VERMEER_CPUID));
	ATF_CHECK(!ibs_zen3_errata_cpuid_is_zen3_b0(ZEN5_CPUID));
	ATF_CHECK_EQ(data3, ibs_zen3_errata_sanitize_op_data3(&state, data3));
}

/* TC-UNIT-ZEN3ERR-08: errata-sensitive OP DATA3 masks match bit layout. */
ATF_TC_WITHOUT_HEAD(ibs_unit_zen3_errata_op_data3_masks);
ATF_TC_BODY(ibs_unit_zen3_errata_op_data3_masks, tc)
{

	ATF_CHECK_EQ(IBS_DC_MISS_NO_MAB_ALLOC, (1ULL << 16));
	ATF_CHECK_EQ(IBS_L2_MISS, (1ULL << 20));
	ATF_CHECK_EQ(IBS_SW_PF, (1ULL << 21));
	ATF_CHECK_EQ(IBS_OP_DC_MISS_OPEN_MEM_REQS, (0x3fULL << 26));
	ATF_CHECK_EQ(0ULL, IBS_DC_MISS_NO_MAB_ALLOC & IBS_L2_MISS);
	ATF_CHECK_EQ(0ULL, IBS_SW_PF & IBS_OP_DC_MISS_OPEN_MEM_REQS);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_unit_zen3_errata_detects_affected_cpu);
	ATF_TP_ADD_TC(tp, ibs_unit_zen3_errata_sanitizes_fetch_icmiss);
	ATF_TP_ADD_TC(tp, ibs_unit_zen3_errata_sanitizes_op_data3_nomab);
	ATF_TP_ADD_TC(tp, ibs_unit_zen3_errata_sanitizes_op_data3_swpf);
	ATF_TP_ADD_TC(tp, ibs_unit_zen4_unaffected_not_sanitized);
	ATF_TP_ADD_TC(tp, ibs_unit_forced_zen3_path_is_software_only);
	ATF_TP_ADD_TC(tp, ibs_unit_default_disabled_and_invalid_inputs);
	ATF_TP_ADD_TC(tp, ibs_unit_zen3_errata_op_data3_masks);
	return (atf_no_error());
}

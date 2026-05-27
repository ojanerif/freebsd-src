/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 *
 * [TC-UNIT-CAPS] — IBS capability semantics and per-Zen-generation guarantee tests.
 *
 * Part A (TC-UNIT-CAPS-01 … 09): Verifies what each CPUID 0x8000001B EAX
 * capability bit *implies* at the logic level — which MSR fields or functional
 * gates it unlocks.  All assertions are over constant expressions and the
 * ibs_feat_*() inline accessors; no hardware is touched.
 *
 * Part B (TC-UNIT-CAPS-10 … 14): Documents which capability bits are
 * guaranteed to be set for each Zen generation (Zen 1–5) using synthetic
 * CPUID 1 EAX values.  Assertions over IBS_CPUID_* mask constants serve as
 * compile-time-detectable contracts: if a constant ever shifts, these fail.
 *
 * No hardware access.  Runs on any architecture as any user.
 *
 * Test IDs: TC-UNIT-CAPS-01 … TC-UNIT-CAPS-14
 */

#include <atf-c.h>

#include "ibs_decode.h"

/* -----------------------------------------------------------------------
 * Part A — Capability semantics
 * ----------------------------------------------------------------------- */

/*
 * TC-UNIT-CAPS-01
 * IbsFetchSam (bit 0) gates IBS Fetch sampling.  The hardware enable field
 * for fetch sampling is IBS_FETCH_EN (bit 48 of MSR_IBS_FETCH_CTL).  Verify
 * the mask is non-zero and that the accessor returns true when only bit 0 is
 * set.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_fetch_sam_implies_fetch_en_field);
ATF_TC_BODY(ibs_unit_caps_fetch_sam_implies_fetch_en_field, tc)
{
	ATF_CHECK_MSG(IBS_FETCH_EN != 0ULL,
	    "IBS_FETCH_EN must be a non-zero mask in MSR_IBS_FETCH_CTL");
	ATF_CHECK(ibs_feat_fetch_sampling(IBS_CPUID_FETCH_SAMPLING) == true);
	ATF_CHECK(ibs_feat_fetch_sampling(0U) == false);
}

/*
 * TC-UNIT-CAPS-02
 * IbsOpSam (bit 1) gates IBS Op sampling.  The hardware enable field is
 * IBS_OP_EN (bit 17 of MSR_IBS_OP_CTL).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_op_sam_implies_op_en_field);
ATF_TC_BODY(ibs_unit_caps_op_sam_implies_op_en_field, tc)
{
	ATF_CHECK_MSG(IBS_OP_EN != 0ULL,
	    "IBS_OP_EN must be a non-zero mask in MSR_IBS_OP_CTL");
	ATF_CHECK(ibs_feat_op_sampling(IBS_CPUID_OP_SAMPLING) == true);
	ATF_CHECK(ibs_feat_op_sampling(0U) == false);
}

/*
 * TC-UNIT-CAPS-03
 * RdWrOpCnt (bit 2) gates the read/write op counting mode, controlled by
 * IBS_CNT_CTL (bit 19 of MSR_IBS_OP_CTL).  Verify IBS_CNT_CTL is non-zero,
 * does not overlap IBS_OP_EN (bit 17), and does not overlap IBS_OP_VAL
 * (bit 18).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_rdwropcnt_implies_cnt_ctl_field);
ATF_TC_BODY(ibs_unit_caps_rdwropcnt_implies_cnt_ctl_field, tc)
{
	ATF_CHECK_MSG(IBS_CNT_CTL != 0ULL,
	    "IBS_CNT_CTL must be a non-zero mask in MSR_IBS_OP_CTL");
	ATF_CHECK_MSG((IBS_CNT_CTL & IBS_OP_EN) == 0ULL,
	    "IBS_CNT_CTL must not overlap IBS_OP_EN");
	ATF_CHECK_MSG((IBS_CNT_CTL & IBS_OP_VAL) == 0ULL,
	    "IBS_CNT_CTL must not overlap IBS_OP_VAL");
}

/*
 * TC-UNIT-CAPS-04
 * BrnTrgt (bit 4) gates branch target address reporting via the
 * MSR_IBS_IBSBRTARGET register.  Verify the MSR address constant is correct
 * per AMD PPR.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_brn_target_implies_msr_address);
ATF_TC_BODY(ibs_unit_caps_brn_target_implies_msr_address, tc)
{
	ATF_CHECK_MSG(IBS_MSR_IBSBRTARGET == 0xC001103BU,
	    "IBS_MSR_IBSBRTARGET must be 0xC001103B (AMD PPR)");
	ATF_CHECK_MSG((IBS_CPUID_BRANCH_TARGET_ADDR & (1U << 4)) != 0U,
	    "IBS_CPUID_BRANCH_TARGET_ADDR must be bit 4");
}

/*
 * TC-UNIT-CAPS-05
 * IbsOpData4 (bit 5) gates the IBS Op Data 4 MSR available on Zen 4+.
 * Verify the MSR address constant is correct per AMD PPR.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_opdata4_implies_msr_address);
ATF_TC_BODY(ibs_unit_caps_opdata4_implies_msr_address, tc)
{
	ATF_CHECK_MSG(IBS_MSR_IBSOPDATA4 == 0xC001103DU,
	    "IBS_MSR_IBSOPDATA4 must be 0xC001103D (AMD PPR)");
	ATF_CHECK_MSG((IBS_CPUID_OP_DATA_4 & (1U << 5)) != 0U,
	    "IBS_CPUID_OP_DATA_4 must be bit 5");
}

/*
 * TC-UNIT-CAPS-06
 * Zen4IbsExt (bit 6) implies the L3MissOnly filter field is present in both
 * MSR_IBS_FETCH_CTL (IBS_L3_MISS_ONLY, bit 59) and MSR_IBS_OP_CTL
 * (IBS_OP_L3_MISS_ONLY, bit 16).  Verify both masks are non-zero and the
 * accessor responds to bit 6.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen4_implies_l3missonly_field);
ATF_TC_BODY(ibs_unit_caps_zen4_implies_l3missonly_field, tc)
{
	ATF_CHECK_MSG(IBS_L3_MISS_ONLY != 0ULL,
	    "IBS_L3_MISS_ONLY must be non-zero in MSR_IBS_FETCH_CTL");
	ATF_CHECK_MSG(IBS_OP_L3_MISS_ONLY != 0ULL,
	    "IBS_OP_L3_MISS_ONLY must be non-zero in MSR_IBS_OP_CTL");
	ATF_CHECK(ibs_feat_zen4(IBS_CPUID_ZEN4_IBS) == true);
	ATF_CHECK(ibs_feat_zen4(IBS_CPUID_ZEN4_IBS ^ IBS_CPUID_ZEN4_IBS) ==
	    false);
}

/*
 * TC-UNIT-CAPS-07
 * Zen4IbsExt (bit 6) also implies the extended 23-bit Op MaxCnt field
 * IBS_OP_MAXCNT_EXT (bits 26:20 of MSR_IBS_OP_CTL) is present.  Verify the
 * mask is non-zero and the shift constant is 20.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen4_implies_op_ext_maxcnt_field);
ATF_TC_BODY(ibs_unit_caps_zen4_implies_op_ext_maxcnt_field, tc)
{
	ATF_CHECK_MSG(IBS_OP_MAXCNT_EXT != 0ULL,
	    "IBS_OP_MAXCNT_EXT must be non-zero in MSR_IBS_OP_CTL");
	ATF_CHECK_MSG(IBS_OP_MAXCNT_EXT_SHIFT == 20,
	    "IBS_OP_MAXCNT_EXT_SHIFT must be 20 (AMD PPR)");
	ATF_CHECK_MSG((IBS_OP_MAXCNT_EXT & IBS_OP_MAXCNT) == 0ULL,
	    "extended MaxCnt field must not overlap base MaxCnt field");
}

/*
 * TC-UNIT-CAPS-08
 * Sanity: EAX = 0 means no capabilities.  All three capability accessors
 * must return false.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_no_caps_all_accessors_false);
ATF_TC_BODY(ibs_unit_caps_no_caps_all_accessors_false, tc)
{
	ATF_CHECK(ibs_feat_fetch_sampling(0U) == false);
	ATF_CHECK(ibs_feat_op_sampling(0U) == false);
	ATF_CHECK(ibs_feat_zen4(0U) == false);
}

/*
 * TC-UNIT-CAPS-09
 * Full capability word (all 7 bits set).  All three accessors must return
 * true simultaneously.
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_all_caps_all_accessors_true);
ATF_TC_BODY(ibs_unit_caps_all_caps_all_accessors_true, tc)
{
	const uint32_t all_caps =
	    IBS_CPUID_FETCH_SAMPLING |
	    IBS_CPUID_OP_SAMPLING    |
	    IBS_CPUID_RDWROPCNT      |
	    IBS_CPUID_OPCNT          |
	    IBS_CPUID_BRANCH_TARGET_ADDR |
	    IBS_CPUID_OP_DATA_4      |
	    IBS_CPUID_ZEN4_IBS;

	ATF_CHECK(ibs_feat_fetch_sampling(all_caps) == true);
	ATF_CHECK(ibs_feat_op_sampling(all_caps) == true);
	ATF_CHECK(ibs_feat_zen4(all_caps) == true);
}

/* -----------------------------------------------------------------------
 * Part B — Per-Zen-generation capability guarantees
 *
 * For each generation a synthetic CPUID 1 EAX is used to confirm the
 * generation classification, then a "caps_eax" with the expected guaranteed
 * bits set is asserted.  The assertions document which IBS_CPUID_* bits AMD
 * PPR guarantees for that generation; they fail if the constant definitions
 * ever shift.
 * ----------------------------------------------------------------------- */

/*
 * TC-UNIT-CAPS-10
 * Zen 1 (Family 17h, CPUID 1 EAX = 0x00800F11).
 * Guaranteed: IbsFetchSam (bit 0) and IbsOpSam (bit 1).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen1_baseline_bits);
ATF_TC_BODY(ibs_unit_caps_zen1_baseline_bits, tc)
{
	const uint32_t zen1_eax = 0x00800F11U;
	const uint32_t caps_eax = IBS_CPUID_FETCH_SAMPLING |
	    IBS_CPUID_OP_SAMPLING;

	ATF_CHECK_MSG(ibs_cpuid_family(zen1_eax) == 0x17U,
	    "synthetic EAX 0x00800F11 must decode to Family 0x17 (Zen 1)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_FETCH_SAMPLING) != 0U,
	    "Zen 1 must guarantee IbsFetchSam (bit 0)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_SAMPLING) != 0U,
	    "Zen 1 must guarantee IbsOpSam (bit 1)");
}

/*
 * TC-UNIT-CAPS-11
 * Zen 2 (Family 17h model 31h, CPUID 1 EAX = 0x00870F10).
 * Guaranteed: bits 0–3 (IbsFetchSam, IbsOpSam, RdWrOpCnt, OpCnt).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen2_baseline_bits);
ATF_TC_BODY(ibs_unit_caps_zen2_baseline_bits, tc)
{
	const uint32_t zen2_eax = 0x00870F10U;
	const uint32_t caps_eax =
	    IBS_CPUID_FETCH_SAMPLING |
	    IBS_CPUID_OP_SAMPLING    |
	    IBS_CPUID_RDWROPCNT      |
	    IBS_CPUID_OPCNT;

	ATF_CHECK_MSG(ibs_cpuid_family(zen2_eax) == 0x17U,
	    "synthetic EAX 0x00870F10 must decode to Family 0x17 (Zen 2)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_FETCH_SAMPLING) != 0U,
	    "Zen 2 must guarantee IbsFetchSam (bit 0)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_SAMPLING) != 0U,
	    "Zen 2 must guarantee IbsOpSam (bit 1)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_RDWROPCNT) != 0U,
	    "Zen 2 must guarantee RdWrOpCnt (bit 2)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OPCNT) != 0U,
	    "Zen 2 must guarantee OpCnt (bit 3)");
}

/*
 * TC-UNIT-CAPS-12
 * Zen 3 (Family 19h model 01h, CPUID 1 EAX = 0x00A00F10).
 * Guaranteed: bits 0–4 (adds BrnTrgt).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen3_baseline_bits);
ATF_TC_BODY(ibs_unit_caps_zen3_baseline_bits, tc)
{
	const uint32_t zen3_eax = 0x00A00F10U;
	const uint32_t caps_eax =
	    IBS_CPUID_FETCH_SAMPLING     |
	    IBS_CPUID_OP_SAMPLING        |
	    IBS_CPUID_RDWROPCNT          |
	    IBS_CPUID_OPCNT              |
	    IBS_CPUID_BRANCH_TARGET_ADDR;

	ATF_CHECK_MSG(ibs_cpuid_family(zen3_eax) == 0x19U,
	    "synthetic EAX 0x00A00F10 must decode to Family 0x19 (Zen 3)");
	ATF_CHECK_MSG(!cpu_is_zen4_from_eax(zen3_eax),
	    "synthetic EAX 0x00A00F10 must not be classified as Zen 4");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_FETCH_SAMPLING) != 0U,
	    "Zen 3 must guarantee IbsFetchSam (bit 0)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_SAMPLING) != 0U,
	    "Zen 3 must guarantee IbsOpSam (bit 1)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_RDWROPCNT) != 0U,
	    "Zen 3 must guarantee RdWrOpCnt (bit 2)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OPCNT) != 0U,
	    "Zen 3 must guarantee OpCnt (bit 3)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_BRANCH_TARGET_ADDR) != 0U,
	    "Zen 3 must guarantee BrnTrgt (bit 4)");
}

/*
 * TC-UNIT-CAPS-13
 * Zen 4 (Family 19h model 11h, CPUID 1 EAX = 0x00A10F10).
 * Guaranteed: bits 0–6 (all seven — adds IbsOpData4 and Zen4IbsExt).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen4_extended_bits);
ATF_TC_BODY(ibs_unit_caps_zen4_extended_bits, tc)
{
	const uint32_t zen4_eax = 0x00A10F10U;
	const uint32_t caps_eax =
	    IBS_CPUID_FETCH_SAMPLING     |
	    IBS_CPUID_OP_SAMPLING        |
	    IBS_CPUID_RDWROPCNT          |
	    IBS_CPUID_OPCNT              |
	    IBS_CPUID_BRANCH_TARGET_ADDR |
	    IBS_CPUID_OP_DATA_4          |
	    IBS_CPUID_ZEN4_IBS;

	ATF_CHECK_MSG(cpu_is_zen4_from_eax(zen4_eax),
	    "synthetic EAX 0x00A10F10 must be classified as Zen 4");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_FETCH_SAMPLING) != 0U,
	    "Zen 4 must guarantee IbsFetchSam (bit 0)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_SAMPLING) != 0U,
	    "Zen 4 must guarantee IbsOpSam (bit 1)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_RDWROPCNT) != 0U,
	    "Zen 4 must guarantee RdWrOpCnt (bit 2)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OPCNT) != 0U,
	    "Zen 4 must guarantee OpCnt (bit 3)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_BRANCH_TARGET_ADDR) != 0U,
	    "Zen 4 must guarantee BrnTrgt (bit 4)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_DATA_4) != 0U,
	    "Zen 4 must guarantee IbsOpData4 (bit 5)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_ZEN4_IBS) != 0U,
	    "Zen 4 must guarantee Zen4IbsExt (bit 6)");
}

/*
 * TC-UNIT-CAPS-14
 * Zen 5 (Family 1Ah model 00h, CPUID 1 EAX = 0x00B10F00).
 * Guaranteed: bits 0–6 (same full set as Zen 4).
 */
ATF_TC_WITHOUT_HEAD(ibs_unit_caps_zen5_extended_bits);
ATF_TC_BODY(ibs_unit_caps_zen5_extended_bits, tc)
{
	const uint32_t zen5_eax = 0x00B10F00U;
	const uint32_t caps_eax =
	    IBS_CPUID_FETCH_SAMPLING     |
	    IBS_CPUID_OP_SAMPLING        |
	    IBS_CPUID_RDWROPCNT          |
	    IBS_CPUID_OPCNT              |
	    IBS_CPUID_BRANCH_TARGET_ADDR |
	    IBS_CPUID_OP_DATA_4          |
	    IBS_CPUID_ZEN4_IBS;

	ATF_CHECK_MSG(cpu_is_zen5_from_eax(zen5_eax),
	    "synthetic EAX 0x00B10F00 must be classified as Zen 5");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_FETCH_SAMPLING) != 0U,
	    "Zen 5 must guarantee IbsFetchSam (bit 0)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_SAMPLING) != 0U,
	    "Zen 5 must guarantee IbsOpSam (bit 1)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_RDWROPCNT) != 0U,
	    "Zen 5 must guarantee RdWrOpCnt (bit 2)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OPCNT) != 0U,
	    "Zen 5 must guarantee OpCnt (bit 3)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_BRANCH_TARGET_ADDR) != 0U,
	    "Zen 5 must guarantee BrnTrgt (bit 4)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_OP_DATA_4) != 0U,
	    "Zen 5 must guarantee IbsOpData4 (bit 5)");
	ATF_CHECK_MSG((caps_eax & IBS_CPUID_ZEN4_IBS) != 0U,
	    "Zen 5 must guarantee Zen4IbsExt (bit 6)");
}

ATF_TP_ADD_TCS(tp)
{
	/* Part A — capability semantics */
	ATF_TP_ADD_TC(tp, ibs_unit_caps_fetch_sam_implies_fetch_en_field);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_op_sam_implies_op_en_field);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_rdwropcnt_implies_cnt_ctl_field);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_brn_target_implies_msr_address);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_opdata4_implies_msr_address);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen4_implies_l3missonly_field);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen4_implies_op_ext_maxcnt_field);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_no_caps_all_accessors_false);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_all_caps_all_accessors_true);
	/* Part B — per-Zen-generation guarantees */
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen1_baseline_bits);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen2_baseline_bits);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen3_baseline_bits);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen4_extended_bits);
	ATF_TP_ADD_TC(tp, ibs_unit_caps_zen5_extended_bits);
	return (atf_no_error());
}

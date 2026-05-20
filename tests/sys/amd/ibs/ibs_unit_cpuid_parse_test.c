/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 *
 * [TC-UNIT-CPUID] — CPU family/model/stepping parsing unit tests.
 *
 * Tests ibs_cpuid_family(), ibs_cpuid_model(), ibs_cpuid_stepping(),
 * cpu_is_zen4_from_eax(), and cpu_is_zen5_from_eax() from ibs_decode.h
 * against known EAX values from the AMD PPR.
 *
 * No hardware access.  Runs on any architecture.
 *
 * Test IDs: TC-UNIT-CPUID-01 … TC-UNIT-CPUID-12
 *
 * EAX encoding (CPUID leaf 1):
 *   bits  3: 0  Stepping
 *   bits  7: 4  BaseModel
 *   bits 11: 8  BaseFamily
 *   bits 19:16  ExtendedModel
 *   bits 27:20  ExtendedFamily
 *
 * Effective family = BaseFamily + ExtendedFamily
 * Effective model  = (ExtendedModel << 4) | BaseModel
 */

#include <atf-c.h>

#include "ibs_decode.h"

/* TC-UNIT-CPUID-01: Zen 1 (Family 0x17, model 0x01) */
ATF_TC_WITHOUT_HEAD(ibs_unit_family_zen1);
ATF_TC_BODY(ibs_unit_family_zen1, tc)
{
	/* EAX 0x00800F11:
	 *   ExtFam=0x08, BaseFam=0xF → family = 0x08+0x0F = 0x17
	 *   ExtMod=0x1,  BaseMod=0x1 → model  = 0x11 */
	ATF_CHECK_EQ(ibs_cpuid_family(0x00800F11U), 0x17U);
}

/* TC-UNIT-CPUID-02: Zen 2 is also Family 0x17 */
ATF_TC_WITHOUT_HEAD(ibs_unit_family_zen2);
ATF_TC_BODY(ibs_unit_family_zen2, tc)
{
	/* EAX 0x00870F10: ExtFam=0x08, BaseFam=0xF → 0x17 */
	ATF_CHECK_EQ(ibs_cpuid_family(0x00870F10U), 0x17U);
}

/* TC-UNIT-CPUID-03: Zen 3 — Family 0x19 */
ATF_TC_WITHOUT_HEAD(ibs_unit_family_zen3);
ATF_TC_BODY(ibs_unit_family_zen3, tc)
{
	/* EAX 0x00A00F10: ExtFam=0x0A, BaseFam=0xF → 0x19 */
	ATF_CHECK_EQ(ibs_cpuid_family(0x00A00F10U), 0x19U);
}

/* TC-UNIT-CPUID-04: Zen 4 — Family 0x19, higher model */
ATF_TC_WITHOUT_HEAD(ibs_unit_family_zen4);
ATF_TC_BODY(ibs_unit_family_zen4, tc)
{
	/* EAX 0x00A20F10: ExtFam=0x0A, BaseFam=0xF → 0x19 */
	ATF_CHECK_EQ(ibs_cpuid_family(0x00A20F10U), 0x19U);
}

/* TC-UNIT-CPUID-05: Zen 5 — Family 0x1A */
ATF_TC_WITHOUT_HEAD(ibs_unit_family_zen5);
ATF_TC_BODY(ibs_unit_family_zen5, tc)
{
	/* EAX 0x00B10F00: ExtFam=0x0B, BaseFam=0xF → 0x1A */
	ATF_CHECK_EQ(ibs_cpuid_family(0x00B10F00U), 0x1AU);
}

/* TC-UNIT-CPUID-06: non-AMD family (Intel Core i7-4770, family 6) */
ATF_TC_WITHOUT_HEAD(ibs_unit_family_intel);
ATF_TC_BODY(ibs_unit_family_intel, tc)
{
	/* EAX 0x000306C3: ExtFam=0, BaseFam=0x6 → 0x06 */
	ATF_CHECK_EQ(ibs_cpuid_family(0x000306C3U), 0x06U);
}

/* TC-UNIT-CPUID-07: model extraction — Family 19h model 0x21 */
ATF_TC_WITHOUT_HEAD(ibs_unit_model_extraction);
ATF_TC_BODY(ibs_unit_model_extraction, tc)
{
	/* EAX 0x00A20F10:
	 *   ExtMod = (bits 19:16) = 0x2
	 *   BaseMod = (bits 7:4)  = 0x1
	 *   model = (0x2 << 4) | 0x1 = 0x21 */
	ATF_CHECK_EQ(ibs_cpuid_model(0x00A20F10U), 0x21U);
}

/* TC-UNIT-CPUID-08: stepping extraction */
ATF_TC_WITHOUT_HEAD(ibs_unit_stepping_extraction);
ATF_TC_BODY(ibs_unit_stepping_extraction, tc)
{
	/* EAX 0x00A20F12: bits 3:0 = 0x2 */
	ATF_CHECK_EQ(ibs_cpuid_stepping(0x00A20F12U), 2U);
}

/* TC-UNIT-CPUID-09: cpu_is_zen4_from_eax() returns true for Zen 4 EAX */
ATF_TC_WITHOUT_HEAD(ibs_unit_is_zen4_true);
ATF_TC_BODY(ibs_unit_is_zen4_true, tc)
{
	/* Family 0x19, model 0x11 → Zen 4 */
	ATF_CHECK(cpu_is_zen4_from_eax(0x00A10F10U) == true);
}

/* TC-UNIT-CPUID-10: cpu_is_zen4_from_eax() returns false for Zen 3 EAX */
ATF_TC_WITHOUT_HEAD(ibs_unit_is_zen4_false_zen3);
ATF_TC_BODY(ibs_unit_is_zen4_false_zen3, tc)
{
	/* Family 0x19, model 0x21 → Zen 3, not Zen 4 */
	ATF_CHECK(cpu_is_zen4_from_eax(0x00A20F10U) == false);
}

/* TC-UNIT-CPUID-11: cpu_is_zen5_from_eax() returns true for Zen 5 EAX */
ATF_TC_WITHOUT_HEAD(ibs_unit_is_zen5_true);
ATF_TC_BODY(ibs_unit_is_zen5_true, tc)
{
	/* Family 0x1A → Zen 5 */
	ATF_CHECK(cpu_is_zen5_from_eax(0x00B10F00U) == true);
}

/* TC-UNIT-CPUID-12: cpu_is_zen5_from_eax() returns false for Zen 6 EAX */
ATF_TC_WITHOUT_HEAD(ibs_unit_is_zen5_false_zen6);
ATF_TC_BODY(ibs_unit_is_zen5_false_zen6, tc)
{
	/* Family 0x1A, model 0x50 → Zen 6, not Zen 5 */
	ATF_CHECK(cpu_is_zen5_from_eax(0x00B50F00U) == false);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_unit_family_zen1);
	ATF_TP_ADD_TC(tp, ibs_unit_family_zen2);
	ATF_TP_ADD_TC(tp, ibs_unit_family_zen3);
	ATF_TP_ADD_TC(tp, ibs_unit_family_zen4);
	ATF_TP_ADD_TC(tp, ibs_unit_family_zen5);
	ATF_TP_ADD_TC(tp, ibs_unit_family_intel);
	ATF_TP_ADD_TC(tp, ibs_unit_model_extraction);
	ATF_TP_ADD_TC(tp, ibs_unit_stepping_extraction);
	ATF_TP_ADD_TC(tp, ibs_unit_is_zen4_true);
	ATF_TP_ADD_TC(tp, ibs_unit_is_zen4_false_zen3);
	ATF_TP_ADD_TC(tp, ibs_unit_is_zen5_true);
	ATF_TP_ADD_TC(tp, ibs_unit_is_zen5_false_zen6);
	return (atf_no_error());
}

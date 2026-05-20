/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 *
 * Pure IBS decode helpers — no hardware I/O, no OS-specific headers.
 * Include only <stdint.h> and <stdbool.h>.  Safe to use on any platform.
 *
 * These helpers are the prerequisite for the [TC-UNIT-*] unit test tier.
 * Hardware accessors (read_msr, write_msr, cpu_supports_ibs, …) remain
 * in ibs_utils.h and must NOT be included here.
 */

#ifndef _IBS_DECODE_H_
#define _IBS_DECODE_H_

#include <stdbool.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * MSR address constants (pure numeric — no OS headers required)
 * ----------------------------------------------------------------------- */
#define IBS_MSR_FETCH_CTL		0xC0011030U
#define IBS_MSR_FETCH_LINADDR		0xC0011031U
#define IBS_MSR_FETCH_PHYSADDR		0xC0011032U
#define IBS_MSR_OP_CTL			0xC0011033U
#define IBS_MSR_OP_RIP			0xC0011034U
#define IBS_MSR_OP_DATA1		0xC0011035U
#define IBS_MSR_OP_DATA2		0xC0011036U
#define IBS_MSR_OP_DATA3		0xC0011037U
#define IBS_MSR_DC_LINADDR		0xC0011038U
#define IBS_MSR_DC_PHYSADDR		0xC0011039U
#define IBS_MSR_IBSCTL			0xC001103AU
#define IBS_MSR_IBSBRTARGET		0xC001103BU
#define IBS_MSR_ICIBSEXTDCTL		0xC001103CU
#define IBS_MSR_IBSOPDATA4		0xC001103DU

/* -----------------------------------------------------------------------
 * CPUID constants
 * ----------------------------------------------------------------------- */
#define IBS_CPUID_IBSID			0x8000001BU
#define IBS_CPUID_AMD_FEATURES		0x80000001U

/* IBS feature flags in CPUID 0x8000001B EAX */
#define IBS_CPUID_FETCH_SAMPLING	(1U << 0)	/* IbsFetchSam */
#define IBS_CPUID_OP_SAMPLING		(1U << 1)	/* IbsOpSam */
#define IBS_CPUID_RDWROPCNT		(1U << 2)	/* RdWrOpCnt */
#define IBS_CPUID_OPCNT			(1U << 3)	/* OpCnt */
#define IBS_CPUID_BRANCH_TARGET_ADDR	(1U << 4)	/* BrnTrgt */
#define IBS_CPUID_OP_DATA_4		(1U << 5)	/* IbsOpData4 */
#define IBS_CPUID_ZEN4_IBS		(1U << 6)	/* Zen4IbsExt */

/* AMD IBS present flag in CPUID 0x80000001 ECX */
#define IBS_CPUID_AMDID2_IBS		0x00000400U	/* ECX[10] */

/* -----------------------------------------------------------------------
 * IBS Fetch Control (MSR_IBS_FETCH_CTL = 0xC0011030) bit masks
 * ----------------------------------------------------------------------- */
#define IBS_FETCH_MAXCNT		0x000000000000ffffULL	/* Bits 0-15 */
#define IBS_FETCH_CNT			0x00000000ffff0000ULL	/* Bits 16-31 */
#define IBS_FETCH_LAT			0x0000ffff00000000ULL	/* Bits 32-47 */
#define IBS_FETCH_EN			0x0001000000000000ULL	/* Bit 48 */
#define IBS_FETCH_VAL			0x0002000000000000ULL	/* Bit 49 */
#define IBS_FETCH_COMP			0x0004000000000000ULL	/* Bit 50 */
#define IBS_IC_MISS			0x0008000000000000ULL	/* Bit 51 */
#define IBS_PHY_ADDR_VALID		0x0010000000000000ULL	/* Bit 52 */
#define IBS_L1TLB_PGSZ			0x0060000000000000ULL	/* Bits 53-54 */
#define IBS_L1TLB_PGSZ_SHIFT		53
#define IBS_L1TLB_MISS			0x0080000000000000ULL	/* Bit 55 */
#define IBS_L2TLB_MISS			0x0100000000000000ULL	/* Bit 56 */
#define IBS_RAND_EN			0x0200000000000000ULL	/* Bit 57 */
#define IBS_FETCH_L2_MISS		0x0400000000000000ULL	/* Bit 58 */
#define IBS_L3_MISS_ONLY		0x0800000000000000ULL	/* Bit 59 */

/* -----------------------------------------------------------------------
 * IBS Op Control (MSR_IBS_OP_CTL = 0xC0011033) bit masks
 * ----------------------------------------------------------------------- */
#define IBS_OP_MAXCNT			0x000000000000ffffULL	/* Bits 0-15 */
#define IBS_OP_L3_MISS_ONLY		0x0000000000010000ULL	/* Bit 16 */
#define IBS_OP_EN			0x0000000000020000ULL	/* Bit 17 */
#define IBS_OP_VAL			0x0000000000040000ULL	/* Bit 18 */
#define IBS_CNT_CTL			0x0000000000080000ULL	/* Bit 19 */
#define IBS_OP_MAXCNT_EXT		0x0000000007f00000ULL	/* Bits 20-26 */
#define IBS_OP_MAXCNT_EXT_SHIFT		20
#define IBS_OP_CURCNT			0x07ffffff00000000ULL	/* Bits 32-58 */
#define IBS_LDLAT_THRSH			0x7800000000000000ULL	/* Bits 59-62 */
#define IBS_LDLAT_THRSH_SHIFT		59
#define IBS_LDLAT_EN			0x8000000000000000ULL	/* Bit 63 */

/* -----------------------------------------------------------------------
 * IBS Op Data 1 (MSR 0xC0011035) bit masks
 * ----------------------------------------------------------------------- */
#define IBS_COMP_TO_RET_CTR		0x000000000000ffffULL	/* Bits 0-15 */
#define IBS_TAG_TO_RET_CTR		0x00000000ffff0000ULL	/* Bits 16-31 */
#define IBS_OP_RETURN			0x0000000400000000ULL	/* Bit 34 */
#define IBS_OP_BRN_TAKEN		0x0000000800000000ULL	/* Bit 35 */
#define IBS_OP_BRN_MISP			0x0000001000000000ULL	/* Bit 36 */
#define IBS_OP_BRN_RET			0x0000002000000000ULL	/* Bit 37 */

/* -----------------------------------------------------------------------
 * IBS Op Data 2 (MSR 0xC0011036) bit masks
 * ----------------------------------------------------------------------- */
#define IBS_DATA_SRC_LO			0x0000000000000007ULL	/* Bits 0-2 */
#define IBS_RMT_NODE			0x0000000000000010ULL	/* Bit 4 */
#define IBS_CACHE_HIT_ST		0x0000000000000020ULL	/* Bit 5 */
#define IBS_DATA_SRC_HI			0x00000000000000c0ULL	/* Bits 6-7 */

/* -----------------------------------------------------------------------
 * IBS Op Data 3 (MSR 0xC0011037) bit masks
 * ----------------------------------------------------------------------- */
#define IBS_LD_OP			0x0000000000000001ULL	/* Bit 0 */
#define IBS_ST_OP			0x0000000000000002ULL	/* Bit 1 */
#define IBS_DC_MISS			0x0000000000000080ULL	/* Bit 7 */
#define IBS_DC_MISS_NO_MAB_ALLOC	0x0000000000010000ULL	/* Bit 16 */
#define IBS_DC_LIN_ADDR_VALID		0x0000000000020000ULL	/* Bit 17 */
#define IBS_DC_PHY_ADDR_VALID		0x0000000000040000ULL	/* Bit 18 */
#define IBS_L2_MISS			0x0000000000100000ULL	/* Bit 20 */
#define IBS_SW_PF			0x0000000000200000ULL	/* Bit 21 */
#define IBS_OP_DC_MISS_OPEN_MEM_REQS	0x00000000fc000000ULL	/* Bits 26-31 */
#define IBS_DC_MISS_LAT			0x0000ffff00000000ULL	/* Bits 32-47 */

/* Generic MaxCnt mask (applies to both Fetch and Op base MaxCnt) */
#define IBS_MAXCNT_MASK			0x000000000000ffffULL

/* -----------------------------------------------------------------------
 * Pure helper functions — no I/O, no system calls
 * ----------------------------------------------------------------------- */

/* Extract the base MaxCnt field (bits 0-15) from an IBS CTL register value. */
static inline uint64_t
ibs_get_maxcnt(uint64_t ctl_val)
{
	return (ctl_val & IBS_MAXCNT_MASK);
}

/* Set the base MaxCnt field (bits 0-15) in an IBS CTL register value.
 * Preserves all other bits.  Clamps maxcnt to 16 bits. */
static inline uint64_t
ibs_set_maxcnt(uint64_t ctl_val, uint64_t maxcnt)
{
	return ((ctl_val & ~IBS_MAXCNT_MASK) | (maxcnt & IBS_MAXCNT_MASK));
}

/* Convert a MaxCnt value to the equivalent sampling period in CPU cycles.
 * AMD PPR: actual period = MaxCnt × 16. */
static inline uint64_t
ibs_maxcnt_to_period(uint64_t maxcnt)
{
	return (maxcnt << 4);
}

/*
 * Extract the combined 5-bit DataSrc field from MSR_IBS_OP_DATA2.
 *
 * The DataSrc field is split across two non-contiguous bit ranges:
 *   DataSrcLo = bits [2:0]   (IBS_DATA_SRC_LO)
 *   DataSrcHi = bits [7:6]   (IBS_DATA_SRC_HI)
 *
 * Combined result = (DataSrcHi << 3) | DataSrcLo.
 * Note: DataSrcHi sits at register bits [7:6], so the extraction
 * must shift right by 6 (not 3) before combining with the low bits.
 */
static inline uint32_t
ibs_get_data_src(uint64_t op_data2)
{
	uint32_t lo = (uint32_t)(op_data2 & IBS_DATA_SRC_LO);
	uint32_t hi = (uint32_t)((op_data2 & IBS_DATA_SRC_HI) >> 6);

	return ((hi << 3) | lo);
}

/*
 * Extract the CPU family from CPUID leaf 1 EAX.
 * Formula per AMD PPR: family = BaseFamily + ExtendedFamily.
 */
static inline uint32_t
ibs_cpuid_family(uint32_t eax)
{
	return (((eax >> 8) & 0xfU) + ((eax >> 20) & 0xffU));
}

/*
 * Extract the CPU model from CPUID leaf 1 EAX.
 * Formula per AMD PPR: model = (ExtendedModel << 4) | BaseModel.
 */
static inline uint32_t
ibs_cpuid_model(uint32_t eax)
{
	return (((eax >> 4) & 0xfU) | (((eax >> 16) & 0xfU) << 4));
}

/* Extract the CPU stepping from CPUID leaf 1 EAX. */
static inline uint32_t
ibs_cpuid_stepping(uint32_t eax)
{
	return (eax & 0xfU);
}

/*
 * Determine Zen 4 from a raw CPUID 1 EAX value.
 * Family 19h is shared by Zen 3, Zen 3+, and Zen 4, so model range matters.
 */
static inline bool
cpu_is_zen4_from_eax(uint32_t eax)
{
	uint32_t model;

	if (ibs_cpuid_family(eax) != 0x19U)
		return (false);
	model = ibs_cpuid_model(eax);
	return ((model >= 0x10U && model <= 0x1fU) ||
	    (model >= 0x60U && model <= 0x7fU) ||
	    (model >= 0xa0U && model <= 0xafU));
}

/* Determine Zen 5 from a raw CPUID 1 EAX value.  Family 1Ah also has Zen 6. */
static inline bool
cpu_is_zen5_from_eax(uint32_t eax)
{
	uint32_t model;

	if (ibs_cpuid_family(eax) != 0x1aU)
		return (false);
	model = ibs_cpuid_model(eax);
	return (model <= 0x2fU || (model >= 0x40U && model <= 0x4fU) ||
	    (model >= 0x60U && model <= 0x7fU));
}

/*
 * Get the full 23-bit IBS Op MaxCnt from IBSOPCTL.
 *   base[15:0]  = bits 15:0  of opctl
 *   ext[6:0]    = bits 26:20 of opctl
 *   full_maxcnt = (ext << 16) | base
 */
static inline uint64_t
ibs_op_get_full_maxcnt(uint64_t opctl)
{
	uint64_t base = opctl & IBS_OP_MAXCNT;
	uint64_t ext  = (opctl & IBS_OP_MAXCNT_EXT) >> IBS_OP_MAXCNT_EXT_SHIFT;

	return ((ext << 16) | base);
}

/*
 * Set the full 23-bit IBS Op MaxCnt in IBSOPCTL.
 * Clears both IBS_OP_MAXCNT (bits 0-15) and IBS_OP_MAXCNT_EXT (bits 20-26)
 * then writes the new value, preserving all other bits.
 */
static inline uint64_t
ibs_op_set_full_maxcnt(uint64_t opctl, uint64_t maxcnt)
{
	uint64_t base = maxcnt & 0xffffULL;
	uint64_t ext  = (maxcnt >> 16) & 0x7fULL;

	opctl &= ~(IBS_OP_MAXCNT | IBS_OP_MAXCNT_EXT);
	opctl |= base | (ext << IBS_OP_MAXCNT_EXT_SHIFT);
	return (opctl);
}

/* IBS CPUID feature flag accessors (CPUID 0x8000001B EAX). */
static inline bool
ibs_feat_fetch_sampling(uint32_t cpuid_ibsid_eax)
{
	return ((cpuid_ibsid_eax & IBS_CPUID_FETCH_SAMPLING) != 0);
}

static inline bool
ibs_feat_op_sampling(uint32_t cpuid_ibsid_eax)
{
	return ((cpuid_ibsid_eax & IBS_CPUID_OP_SAMPLING) != 0);
}

static inline bool
ibs_feat_zen4(uint32_t cpuid_ibsid_eax)
{
	return ((cpuid_ibsid_eax & IBS_CPUID_ZEN4_IBS) != 0);
}

#endif /* _IBS_DECODE_H_ */

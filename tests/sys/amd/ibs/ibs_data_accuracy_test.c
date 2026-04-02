/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * IBS Data Accuracy Validation Test
 *
 * This test validates the data accuracy of IBS (Instruction-Based Sampling)
 * sample data fields, including DataSrc encodings, Op Data field extraction,
 * and address field validation.
 *
 * Test cases:
 *   ibs_data_src_encodings  - Test DataSrc encoding values (L1, L2, L3, DRAM, etc.)
 *   ibs_data_src_extended   - Test Zen 4+ extended DataSrc encodings
 *   ibs_op_data_fields      - Test Op Data 1/2/3 field extraction
 *   ibs_op_data4_zen4       - Test Op Data 4 fields on Zen 4+ (remote latency, L3 miss)
 *   ibs_fetch_address_fields - Test Fetch linear/physical address fields
 *   ibs_dc_address_fields   - Test DC linear/physical address fields
 *
 * Reference: Linux kernel ibs.c, AMD PPR documentation
 */

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ibs_utils.h"

/*
 * DataSrc encoding constants from IBS_OP_DATA2.
 *
 * The DataSrc field indicates where the data for a load operation came from.
 * It is split into two parts:
 *   - DataSrcLo: bits [2:0] of IBS_OP_DATA2
 *   - DataSrcHi: bits [7:6] of IBS_OP_DATA2
 *
 * Combined value = (DataSrcHi << 3) | DataSrcLo
 *
 * Standard encodings (pre-Zen 4):
 *   0x2 = Local cache hit
 *   0x3 = DRAM
 *   0x4 = Remote cache hit
 *   0x7 = I/O
 *
 * Extended encodings (Zen 4+):
 *   0x1 = Local cache
 *   0x2 = Near CCX cache
 *   0x3 = DRAM
 *   0x5 = Far CCX cache
 *   0x6 = PMEM
 *   0x7 = I/O
 *   0x8 = Extended memory
 *   0xC = Peer agent memory
 */
#define IBS_DATA_SRC_MASK_LO	0x00000007ULL	/* Bits 0-2 */
#define IBS_DATA_SRC_MASK_HI	0x000000C0ULL	/* Bits 6-7 */
#define IBS_DATA_SRC_SHIFT_HI	3		/* Shift for high bits */

/*
 * Helper: Extract the combined DataSrc value from IBS_OP_DATA2.
 */
static inline uint64_t
ibs_get_data_src(uint64_t op_data2)
{
	uint64_t lo, hi;

	lo = op_data2 & IBS_DATA_SRC_MASK_LO;
	hi = (op_data2 & IBS_DATA_SRC_MASK_HI) >> IBS_DATA_SRC_SHIFT_HI;

	return (hi << 3) | lo;
}

/*
 * Helper: Construct IBS_OP_DATA2 value with a given DataSrc encoding.
 */
static inline uint64_t
ibs_set_data_src(uint64_t data_src)
{
	uint64_t lo, hi;

	lo = data_src & 0x7;
	hi = (data_src >> 3) & 0x3;

	return (hi << 6) | lo;
}

/*
 * Helper: Extract Op Data 1 fields.
 */
static inline uint16_t
ibs_get_comp_to_ret_ctr(uint64_t op_data1)
{
	return ((op_data1 & IBS_COMP_TO_RET_CTR) >> 0);
}

static inline uint16_t
ibs_get_tag_to_ret_ctr(uint64_t op_data1)
{
	return ((op_data1 & IBS_TAG_TO_RET_CTR) >> 16);
}

/*
 * Helper: Extract Op Data 3 fields.
 */
static inline uint64_t
ibs_get_op_mem_width(uint64_t op_data3)
{
	return ((op_data3 & IBS_OP_MEM_WIDTH) >> IBS_OP_MEM_WIDTH_SHIFT);
}

static inline uint64_t
ibs_get_dc_miss_lat(uint64_t op_data3)
{
	return ((op_data3 & IBS_DC_MISS_LAT) >> 32);
}

static inline uint64_t
ibs_get_tlb_refill_lat(uint64_t op_data3)
{
	return ((op_data3 & IBS_TLB_REFILL_LAT) >> 48);
}

/*
 * Helper: Extract Op Data 4 fields (Zen 4+).
 */
static inline uint64_t
ibs_get_remote_lat(uint64_t op_data4)
{
	return ((op_data4 & IBS_OP_DATA4_REMOTE_LAT) >> IBS_OP_DATA4_REMOTE_LAT_SHIFT);
}

/*
 * Helper: Extract Fetch address fields.
 */
static inline uint64_t
ibs_get_fetch_lin_addr(uint64_t lin_addr)
{
	/* Linear address is in bits [63:0], page offset in [11:0] */
	return (lin_addr);
}

static inline uint64_t
ibs_get_fetch_phy_addr(uint64_t phy_addr)
{
	/* Physical address valid bit is in IBSFETCHCTL bit 52 */
	return (phy_addr & ~0x0fffULL);
}

/*
 * Helper: Extract DC address fields.
 */
static inline uint64_t
ibs_get_dc_lin_addr(uint64_t lin_addr)
{
	return (lin_addr);
}

static inline uint64_t
ibs_get_dc_phy_addr(uint64_t phy_addr)
{
	return (phy_addr & ~0x0fffULL);
}

/*
 * Test: ibs_data_src_encodings
 *
 * Verify that DataSrc encoding values can be written and read back
 * correctly through IBS_OP_DATA2 MSR.
 *
 * This test validates the standard DataSrc encodings:
 *   - 0x2: Local cache hit
 *   - 0x3: DRAM
 *   - 0x4: Remote cache hit
 *   - 0x7: I/O
 */
ATF_TC(ibs_data_src_encodings);
ATF_TC_HEAD(ibs_data_src_encodings, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS DataSrc encoding values (L1, L2, L3, DRAM, I/O)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_data_src_encodings, tc)
{
	uint64_t original, written, readback;
	int error;
	struct {
		uint64_t data_src;
		const char *desc;
	} test_cases[] = {
		{ 0x2, "Local cache hit" },
		{ 0x3, "DRAM" },
		{ 0x4, "Remote cache hit" },
		{ 0x7, "I/O" },
	};
	size_t i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_DATA, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_DATA: %s",
		    strerror(error));

	for (i = 0; i < nitems(test_cases); i++) {
		/* Write DataSrc encoding */
		written = ibs_set_data_src(test_cases[i].data_src);
		error = write_msr(0, MSR_IBS_OP_DATA, written);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Read back and verify */
		error = read_msr(0, MSR_IBS_OP_DATA, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK_EQ(ibs_get_data_src(readback),
		    test_cases[i].data_src);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_DATA, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_data_src_extended
 *
 * Verify that extended DataSrc encoding values work correctly on
 * Zen 4+ processors. These encodings use the full 4-bit range
 * (bits 0-2 and 6-7 of IBS_OP_DATA2).
 *
 * Extended encodings:
 *   - 0x1: Local cache
 *   - 0x2: Near CCX cache
 *   - 0x3: DRAM
 *   - 0x5: Far CCX cache
 *   - 0x6: PMEM
 *   - 0x7: I/O
 *   - 0x8: Extended memory
 *   - 0xC: Peer agent memory
 */
ATF_TC(ibs_data_src_extended);
ATF_TC_HEAD(ibs_data_src_extended, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS extended DataSrc encodings on Zen 4+");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_data_src_extended, tc)
{
	uint64_t original, written, readback;
	int error;
	struct {
		uint64_t data_src;
		const char *desc;
	} test_cases[] = {
		{ 0x1, "Local cache" },
		{ 0x2, "Near CCX cache" },
		{ 0x3, "DRAM" },
		{ 0x5, "Far CCX cache" },
		{ 0x6, "PMEM" },
		{ 0x7, "I/O" },
		{ 0x8, "Extended memory" },
		{ 0xC, "Peer agent memory" },
	};
	size_t i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_is_zen4())
		atf_tc_skip("CPU is not Zen 4+ (extended DataSrc not supported)");

	error = read_msr(0, MSR_IBS_OP_DATA, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_DATA: %s",
		    strerror(error));

	for (i = 0; i < nitems(test_cases); i++) {
		/* Write extended DataSrc encoding */
		written = ibs_set_data_src(test_cases[i].data_src);
		error = write_msr(0, MSR_IBS_OP_DATA, written);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Read back and verify */
		error = read_msr(0, MSR_IBS_OP_DATA, &readback);
		ATF_REQUIRE_ERRNO(0, error == 0);
		ATF_CHECK_EQ(ibs_get_data_src(readback),
		    test_cases[i].data_src);
	}

	/* Restore original value */
	error = write_msr(0, MSR_IBS_OP_DATA, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_data_fields
 *
 * Verify that Op Data 1/2/3 field extraction works correctly.
 * This test writes known values to the MSRs and verifies that
 * the field extraction helpers return the expected results.
 */
ATF_TC(ibs_op_data_fields);
ATF_TC_HEAD(ibs_op_data_fields, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op Data 1/2/3 field extraction");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_op_data_fields, tc)
{
	uint64_t orig_data1, orig_data2, orig_data3;
	uint64_t written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original values */
	error = read_msr(0, MSR_IBS_OP_DATA, &orig_data1);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_DATA: %s",
		    strerror(error));
	error = read_msr(0, MSR_IBS_OP_DATA2, &orig_data2);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_DATA2: %s",
		    strerror(error));
	error = read_msr(0, MSR_IBS_OP_DATA3, &orig_data3);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_DATA3: %s",
		    strerror(error));

	/*
	 * Test Op Data 1: comp_to_ret_ctr and tag_to_ret_ctr fields.
	 * Write known values and verify extraction.
	 */
	written = (0x1234ULL << 16) | 0x5678ULL; /* tag=0x1234, comp=0x5678 */
	error = write_msr(0, MSR_IBS_OP_DATA, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_DATA, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_comp_to_ret_ctr(readback), 0x5678);
	ATF_CHECK_EQ(ibs_get_tag_to_ret_ctr(readback), 0x1234);

	/*
	 * Test Op Data 2: DataSrc field extraction.
	 */
	written = ibs_set_data_src(0x3); /* DRAM */
	error = write_msr(0, MSR_IBS_OP_DATA2, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_DATA2, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_data_src(readback), 0x3);

	/*
	 * Test Op Data 3: Various field extractions.
	 * Write a value with known fields set.
	 */
	written = (0x100ULL << 32) |	/* dc_miss_lat = 0x100 */
		  (0x200ULL << 48) |	/* tlb_refill_lat = 0x200 */
		  (0x4ULL << 22) |	/* op_mem_width = 4 bytes */
		  IBS_LD_OP;		/* load op */
	error = write_msr(0, MSR_IBS_OP_DATA3, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_OP_DATA3, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_dc_miss_lat(readback), 0x100);
	ATF_CHECK_EQ(ibs_get_tlb_refill_lat(readback), 0x200);
	ATF_CHECK_EQ(ibs_get_op_mem_width(readback), 0x4);
	ATF_CHECK((readback & IBS_LD_OP) != 0);
	ATF_CHECK((readback & IBS_ST_OP) == 0);

	/* Restore original values */
	error = write_msr(0, MSR_IBS_OP_DATA, orig_data1);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_OP_DATA2, orig_data2);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_OP_DATA3, orig_data3);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_op_data4_zen4
 *
 * Verify that Op Data 4 fields work correctly on Zen 4+ processors.
 * Op Data 4 contains remote latency information and L3 miss data.
 *
 * MSR: 0xc001103d (MSR_AMD64_IBSOPDATA4)
 * Fields:
 *   - Bit 0: Data valid
 *   - Bits [16:1]: Remote latency
 */
ATF_TC(ibs_op_data4_zen4);
ATF_TC_HEAD(ibs_op_data4_zen4, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op Data 4 fields on Zen 4+ (remote latency, L3 miss)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_op_data4_zen4, tc)
{
	uint64_t original, written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_is_zen4())
		atf_tc_skip("CPU is not Zen 4+ (Op Data 4 not supported)");

	error = read_msr(0, MSR_AMD64_IBSOPDATA4, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_AMD64_IBSOPDATA4: %s",
		    strerror(error));

	/*
	 * Test Op Data 4: Write a value with known remote latency.
	 * Remote latency is in bits [16:1], valid bit is bit 0.
	 */
	written = (0x123ULL << IBS_OP_DATA4_REMOTE_LAT_SHIFT) |
	    IBS_OP_DATA4_VALID;
	error = write_msr(0, MSR_AMD64_IBSOPDATA4, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Read back and verify */
	error = read_msr(0, MSR_AMD64_IBSOPDATA4, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_remote_lat(readback), 0x123);
	ATF_CHECK((readback & IBS_OP_DATA4_VALID) != 0);

	/* Test with zero remote latency */
	written = IBS_OP_DATA4_VALID;
	error = write_msr(0, MSR_AMD64_IBSOPDATA4, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_AMD64_IBSOPDATA4, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_remote_lat(readback), 0);
	ATF_CHECK((readback & IBS_OP_DATA4_VALID) != 0);

	/* Restore original value */
	error = write_msr(0, MSR_AMD64_IBSOPDATA4, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_fetch_address_fields
 *
 * Verify that Fetch linear and physical address fields can be
 * written and read back correctly.
 *
 * MSRs:
 *   - MSR_IBS_FETCH_LIN_ADDR (0xc0011031): Fetch linear address
 *   - MSR_IBS_FETCH_PHY_ADDR (0xc0011032): Fetch physical address
 */
ATF_TC(ibs_fetch_address_fields);
ATF_TC_HEAD(ibs_fetch_address_fields, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch linear/physical address fields");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_fetch_address_fields, tc)
{
	uint64_t orig_lin, orig_phy;
	uint64_t written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original values */
	error = read_msr(0, MSR_IBS_FETCH_LIN_ADDR, &orig_lin);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_LIN_ADDR: %s",
		    strerror(error));
	error = read_msr(0, MSR_IBS_FETCH_PHY_ADDR, &orig_phy);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_PHY_ADDR: %s",
		    strerror(error));

	/*
	 * Test Fetch linear address.
	 * The linear address is stored in bits [63:0].
	 */
	written = 0x00007fffff001000ULL; /* Typical user-space address */
	error = write_msr(0, MSR_IBS_FETCH_LIN_ADDR, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_LIN_ADDR, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_fetch_lin_addr(readback), written);

	/* Test with kernel-space address */
	written = 0xffffffff81000000ULL;
	error = write_msr(0, MSR_IBS_FETCH_LIN_ADDR, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_LIN_ADDR, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_fetch_lin_addr(readback), written);

	/*
	 * Test Fetch physical address.
	 * The physical address is stored with page offset in bits [11:0].
	 */
	written = 0x0000000012345000ULL;
	error = write_msr(0, MSR_IBS_FETCH_PHY_ADDR, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_FETCH_PHY_ADDR, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_fetch_phy_addr(readback), written);

	/* Restore original values */
	error = write_msr(0, MSR_IBS_FETCH_LIN_ADDR, orig_lin);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_FETCH_PHY_ADDR, orig_phy);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_dc_address_fields
 *
 * Verify that DC (Data Cache) linear and physical address fields
 * can be written and read back correctly.
 *
 * MSRs:
 *   - MSR_IBS_DC_LIN_AD (0xc0011038): DC linear address
 *   - MSR_IBS_DC_PHYS_AD (0xc0011039): DC physical address
 *
 * The DC linear address valid bit is in IBS_OP_DATA3 bit 17.
 * The DC physical address valid bit is in IBS_OP_DATA3 bit 18.
 */
ATF_TC(ibs_dc_address_fields);
ATF_TC_HEAD(ibs_dc_address_fields, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS DC linear/physical address fields");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_dc_address_fields, tc)
{
	uint64_t orig_lin, orig_phy;
	uint64_t written, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original values */
	error = read_msr(0, MSR_IBS_DC_LIN_AD, &orig_lin);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_DC_LIN_AD: %s",
		    strerror(error));
	error = read_msr(0, MSR_IBS_DC_PHYS_AD, &orig_phy);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_DC_PHYS_AD: %s",
		    strerror(error));

	/*
	 * Test DC linear address.
	 */
	written = 0x00007fffffffe000ULL; /* Stack-like address */
	error = write_msr(0, MSR_IBS_DC_LIN_AD, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_DC_LIN_AD, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_dc_lin_addr(readback), written);

	/* Test with heap-like address */
	written = 0x0000555555555000ULL;
	error = write_msr(0, MSR_IBS_DC_LIN_AD, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_DC_LIN_AD, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_dc_lin_addr(readback), written);

	/*
	 * Test DC physical address.
	 */
	written = 0x00000000abcdef000ULL;
	error = write_msr(0, MSR_IBS_DC_PHYS_AD, written);
	ATF_REQUIRE_ERRNO(0, error == 0);

	error = read_msr(0, MSR_IBS_DC_PHYS_AD, &readback);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ibs_get_dc_phy_addr(readback), written);

	/* Restore original values */
	error = write_msr(0, MSR_IBS_DC_LIN_AD, orig_lin);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_DC_PHYS_AD, orig_phy);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_data_src_encodings);
	ATF_TP_ADD_TC(tp, ibs_data_src_extended);
	ATF_TP_ADD_TC(tp, ibs_op_data_fields);
	ATF_TP_ADD_TC(tp, ibs_op_data4_zen4);
	ATF_TP_ADD_TC(tp, ibs_fetch_address_fields);
	ATF_TP_ADD_TC(tp, ibs_dc_address_fields);

	return (atf_no_error());
}

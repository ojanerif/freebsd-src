/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_data_accuracy);
ATF_TC_HEAD(ibs_data_accuracy, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS data accuracy");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_data_accuracy, tc)
{
	uint64_t op_ctl, op_rip;
	volatile int i;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/*
	 * Set IbsOpMaxCnt (bits 15:0 = 0xFFFF) and IbsOpEn (bit 17).
	 * Bits 16, 18 (IbsOpVal), and 19 are left clear: IbsOpVal is
	 * hardware-set and must not be written during enable, otherwise
	 * the read-back appears asserted before any real sample fires.
	 */
	op_ctl = 0xFFFFULL | (1ULL << 17);
	error = write_msr(0, MSR_IBS_OP_CTL, op_ctl);
	ATF_REQUIRE_EQ(error, 0);

	/* Run enough retired ops for IBS to fire at least one sample. */
	for (i = 0; i < 1000000; i++)
		__asm__ __volatile__("nop");

	/*
	 * IbsOpVal (bit 18 of IBS_OP_CTL) is set by hardware when a
	 * sample has been captured.  Verify it before trusting IBS_OP_RIP.
	 */
	error = read_msr(0, MSR_IBS_OP_CTL, &op_ctl);
	ATF_REQUIRE_EQ(error, 0);
	ATF_CHECK((op_ctl & (1ULL << 18)) != 0);

	error = read_msr(0, MSR_IBS_OP_RIP, &op_rip);
	ATF_REQUIRE_EQ(error, 0);
	ATF_CHECK(op_rip != 0);

	/* Disable IBS op sampling. */
	error = write_msr(0, MSR_IBS_OP_CTL, 0);
	ATF_REQUIRE_EQ(error, 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_data_accuracy);
	return (atf_no_error());
}

/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

ATF_TC(ibs_detect);
ATF_TC_HEAD(ibs_detect, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS feature detection");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_detect, tc)
{
	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	ATF_CHECK(cpu_supports_ibs());
}

ATF_TC(ibs_detect_extended);
ATF_TC_HEAD(ibs_detect_extended, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify extended IBS features detection");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_detect_extended, tc)
{
	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Test extended IBS features */
	ATF_CHECK(cpu_ibs_extended());
}

ATF_TC(ibs_detect_zen4);
ATF_TC_HEAD(ibs_detect_zen4, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify Zen 4+ IBS features detection");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_detect_zen4, tc)
{
	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	if (!cpu_is_zen4())
		atf_tc_skip("CPU is not Zen 4+");

	/* Test Zen 4+ specific IBS features */
	uint32_t regs[4];
	int error;

	/* Check for IBS Op Data 4 capability */
	error = do_cpuid_ioctl(0x8000001B, regs);
	ATF_REQUIRE(error == 0);
	ATF_CHECK((regs[0] & IBS_CPUID_OP_DATA_4) != 0);

	/* Check for Zen4 IBS capability */
	ATF_CHECK((regs[0] & IBS_CPUID_ZEN4_IBS) != 0);
}

ATF_TC(ibs_detect_msr_access);
ATF_TC_HEAD(ibs_detect_msr_access, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS MSR access and Zen 4+ extensions");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_detect_msr_access, tc)
{
	uint64_t val;
	int cpu = 0;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Test basic IBS MSR access */
	ATF_REQUIRE(read_msr(cpu, MSR_AMD64_IBSCTL, &val) == 0);

	/* Test Zen 4+ extended MSRs if available */
	if (cpu_is_zen4()) {
		/* Test IC IBS Extended Control MSR */
		ATF_REQUIRE(read_msr(cpu, MSR_AMD64_ICIBSEXTDCTL, &val) == 0);

		/* Test IBS Op Data 4 MSR */
		ATF_REQUIRE(read_msr(cpu, MSR_AMD64_IBSOPDATA4, &val) == 0);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_detect);
	ATF_TP_ADD_TC(tp, ibs_detect_extended);
	ATF_TP_ADD_TC(tp, ibs_detect_zen4);
	ATF_TP_ADD_TC(tp, ibs_detect_msr_access);
	return (atf_no_error());
}

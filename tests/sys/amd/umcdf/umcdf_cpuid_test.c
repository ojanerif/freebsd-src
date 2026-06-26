/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: davi.chavesazevedo@amd.com
 */

#include <sys/param.h>

#include <atf-c.h>

#include "amd_umcdf_common.h"

ATF_TC(umcdf_cpuid_generation_decode);
ATF_TC_HEAD(umcdf_cpuid_generation_decode, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Decode AMD family/model/stepping and map the CPU to a Zen generation "
	    "before any UMC/DF PMC work");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_cpuid_generation_decode, tc)
{
	struct amd_umcdf_cpu cpu;

	amd_umcdf_skip_unless_known_zen(&cpu);
	printf("AMD family 0x%02x model 0x%02x stepping 0x%x: %s\n",
	    cpu.family, cpu.model, cpu.stepping, amd_umcdf_zen_name(cpu.zen));

	ATF_REQUIRE_MSG(cpu.family >= 0x17,
	    "UMC/DF tests require Zen-class AMD hardware, got family 0x%02x",
	    cpu.family);
	ATF_REQUIRE_MSG(cpu.zen != AMD_UMCDF_ZEN_UNKNOWN,
	    "AMD family/model should be explicitly classified before PMC use");
	ATF_REQUIRE_MSG(cpu.zen != AMD_UMCDF_ZEN_FUTURE,
	    "Future AMD families require PPR review before event assertions");
}

ATF_TC(umcdf_perfmonv2_capability_decode);
ATF_TC_HEAD(umcdf_perfmonv2_capability_decode, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Decode CPUID Fn80000022 PerfMonV2 fields used by FreeBSD hwpmc "
	    "for dynamic DF counter counts and by UMC support work for the "
	    "active UMC mask");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_perfmonv2_capability_decode, tc)
{
	struct amd_umcdf_perfmon_v2 pmv2;
	struct amd_umcdf_cpu cpu;
	int error;

	amd_umcdf_skip_unless_known_zen(&cpu);
	error = amd_umcdf_read_perfmon_v2(&cpu, &pmv2);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID Fn80000022 failed: %s", strerror(error));

	if (!pmv2.leaf_available)
		atf_tc_skip("CPUID Fn80000022 is not available on %s",
		    amd_umcdf_zen_name(cpu.zen));

	printf("Fn80000022: eax=%#x ebx=%#x ecx=%#x edx=%#x\n",
	    pmv2.raw_eax, pmv2.raw_ebx, pmv2.raw_ecx, pmv2.raw_edx);
	printf("PerfMonV2=%s core_pmcs=%u df_pmcs=%u lbr_v2_depth=%u "
	    "active_umc_mask=%#x\n",
	    pmv2.present ? "yes" : "no",
	    pmv2.core_pmcs, pmv2.df_pmcs, pmv2.lbr_v2_depth, pmv2.active_umc_mask);

	ATF_CHECK_MSG(pmv2.core_pmcs <= 16,
	    "unexpected core PMC count %u", pmv2.core_pmcs);
	ATF_CHECK_MSG(pmv2.df_pmcs <= AMD_UMCDF_MAX_DF_PMCS,
	    "unexpected DF PMC count %u", pmv2.df_pmcs);

	if (cpu.zen == AMD_UMCDF_ZEN_4 || cpu.zen == AMD_UMCDF_ZEN_5 ||
	    cpu.zen == AMD_UMCDF_ZEN_6) {
		ATF_CHECK_MSG(pmv2.present,
		    "Zen 4+ should advertise PerfMonV2 when Fn80000022 exists");
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, umcdf_cpuid_generation_decode);
	ATF_TP_ADD_TC(tp, umcdf_perfmonv2_capability_decode);
	return (atf_no_error());
}

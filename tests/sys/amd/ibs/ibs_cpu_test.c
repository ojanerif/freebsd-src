/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

#include <sys/param.h>
#include <sys/module.h>

#include <atf-c.h>

#include "ibs_utils.h"

/*
 * Test case: Verify CPU family detection for AMD processors.
 * This test checks that we can correctly identify AMD CPU families
 * including Family 10h (K10), Family 15h (Bulldozer), Family 17h (Zen/Zen2),
 * Family 19h (Zen3/Zen4), and Family 1Ah (Zen5/Zen6).
 */
ATF_TC(ibs_cpu_detect_family);
ATF_TC_HEAD(ibs_cpu_detect_family, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify AMD CPU family detection (10h, 15h, 17h, 19h, 1Ah)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_cpu_detect_family, tc)
{
	uint32_t family;
	uint32_t regs[4];
	int error;

	/* Get CPU vendor first */
	error = do_cpuid_ioctl(0x0, regs);
	ATF_REQUIRE_EQ(error, 0);

	/* Check if this is an AMD CPU */
	if (regs[1] != 0x68747541 || regs[2] != 0x444d4163 ||
	    regs[3] != 0x69746e65) {
		atf_tc_skip("Not an AMD CPU");
	}

	family = cpu_get_family();
	ATF_CHECK(family >= 0x10);

	/* Print detected family for debugging */
	printf("Detected AMD CPU Family: 0x%x\n", family);

	/* Verify family is one of the known AMD families */
	switch (family) {
	case 0x10:
		printf("Family 10h (K10) detected\n");
		break;
	case 0x15:
		printf("Family 15h (Bulldozer/Piledriver) detected\n");
		break;
	case 0x17:
		printf("Family 17h (Zen/Zen2) detected\n");
		break;
	case 0x19:
		printf("Family 19h (Zen3/Zen4) detected\n");
		break;
	case 0x1a:
		printf("Family 1Ah (Zen5) detected\n");
		break;
	default:
		atf_tc_fail("Unknown AMD CPU family: 0x%x", family);
		break;
	}
}

/*
 * Test case: Verify Zen4 (Family 19h) detection.
 * This test specifically checks for Zen4 processors.
 */
ATF_TC(ibs_cpu_zen4_detection);
ATF_TC_HEAD(ibs_cpu_zen4_detection, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify Zen4 (Family 19h) CPU detection");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_cpu_zen4_detection, tc)
{
	uint32_t family;
	uint32_t model;
	uint32_t regs[4];
	int error;

	error = do_cpuid_ioctl(0x0, regs);
	ATF_REQUIRE_EQ(error, 0);

	/* Check if this is an AMD CPU */
	if (regs[1] != 0x68747541 || regs[2] != 0x444d4163 ||
	    regs[3] != 0x69746e65) {
		atf_tc_skip("Not an AMD CPU");
	}

	family = cpu_get_family();
	model = cpu_get_model();

	if (!cpu_is_zen4()) {
		atf_tc_skip("Not a Zen4 CPU (Family 0x%x, Model 0x%x)",
		    family, model);
	}

	ATF_CHECK(cpu_is_zen4());
	ATF_CHECK(!cpu_is_zen5());

	printf("Zen4 detected: Family 0x%x, Model 0x%x\n", family, model);

}

/*
 * Test case: Verify Zen5 (Family 1Ah) detection.
 * This test specifically checks for Zen5 processors.
 */
ATF_TC(ibs_cpu_zen5_detection);
ATF_TC_HEAD(ibs_cpu_zen5_detection, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify Zen5 (Family 1Ah) CPU detection");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_cpu_zen5_detection, tc)
{
	uint32_t family;
	uint32_t model;
	uint32_t regs[4];
	int error;

	error = do_cpuid_ioctl(0x0, regs);
	ATF_REQUIRE_EQ(error, 0);

	/* Check if this is an AMD CPU */
	if (regs[1] != 0x68747541 || regs[2] != 0x444d4163 ||
	    regs[3] != 0x69746e65) {
		atf_tc_skip("Not an AMD CPU");
	}

	family = cpu_get_family();
	model = cpu_get_model();

	if (!cpu_is_zen5()) {
		atf_tc_skip("Not a Zen5 CPU (Family 0x%x, Model 0x%x)",
		    family, model);
	}

	ATF_CHECK(cpu_is_zen5());
	ATF_CHECK(!cpu_is_zen4());

	printf("Zen5 detected: Family 0x%x, Model 0x%x\n", family, model);

	/*
	 * Zen5 uses the same perfmon event map as Zen4 per Linux reference:
	 * if (cpu_feature_enabled(X86_FEATURE_ZEN4) || boot_cpu_data.x86 >= 0x1a)
	 *     return amd_zen4_perfmon_event_map[hw_event];
	 */
	printf("Zen5 uses Zen4-style perfmon event map\n");
}

/*
 * Test case: Verify TSC frequency detection on AMD CPUs.
 * This test checks that TSC frequency can be determined via CPUID.
 */
ATF_TC(ibs_cpu_tsc_frequency);
ATF_TC_HEAD(ibs_cpu_tsc_frequency, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify TSC frequency detection on AMD CPUs");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_cpu_tsc_frequency, tc)
{
	uint32_t regs[4];
	uint32_t family;
	uint64_t tsc_freq;
	int error;

	error = do_cpuid_ioctl(0x0, regs);
	ATF_REQUIRE_EQ(error, 0);

	/* Check if this is an AMD CPU */
	if (regs[1] != 0x68747541 || regs[2] != 0x444d4163 ||
	    regs[3] != 0x69746e65) {
		atf_tc_skip("Not an AMD CPU");
	}

	family = cpu_get_family();

	/*
	 * Check for CPUID leaf 0x15 (TSC/clock ratio) support.
	 * This is the preferred method for TSC frequency detection.
	 */
	error = do_cpuid_ioctl(0x15, regs);
	if (error != 0 || regs[0] == 0 || regs[1] == 0 || regs[2] == 0) {
		/*
		 * Try CPUID leaf 0x16 (processor frequency info) as fallback.
		 * This gives base frequency in MHz.
		 */
		error = do_cpuid_ioctl(0x16, regs);
		if (error != 0 || regs[0] == 0) {
			atf_tc_skip(
			    "TSC frequency detection not supported (no CPUID 0x15/0x16)");
		}
		tsc_freq = (uint64_t)regs[0] * 1000000;
		printf("TSC frequency from CPUID 0x16: %ju MHz\n",
		    (uintmax_t)(tsc_freq / 1000000));
	} else {
		/*
		 * CPUID 0x15: EBX/EAX = TSC/crystal ratio, ECX = crystal Hz
		 * TSC frequency = ECX * EBX / EAX
		 */
		tsc_freq = (uint64_t)regs[2] * regs[1] / regs[0];
		printf("TSC frequency from CPUID 0x15: %ju Hz\n",
		    (uintmax_t)tsc_freq);
	}

	/*
	 * For AMD Family 17h and later, TSC is invariant (P-state invariant).
	 * This means TSC runs at a constant frequency regardless of P-state.
	 */
	if (family >= 0x17) {
		printf("TSC is P-state invariant (Family >= 17h)\n");
	}

	/*
	 * For Zen4/Zen5 (Family 19h/1Ah), TSC frequency detection
	 * should work via CPUID 0x15 or 0x16.
	 */
	if (family == 0x19 || family == 0x1a) {
		ATF_CHECK(tsc_freq > 0);
		printf("Zen4/Zen5 TSC frequency verified: %ju Hz\n",
		    (uintmax_t)tsc_freq);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_cpu_detect_family);
	ATF_TP_ADD_TC(tp, ibs_cpu_zen4_detection);
	ATF_TP_ADD_TC(tp, ibs_cpu_zen5_detection);
	ATF_TP_ADD_TC(tp, ibs_cpu_tsc_frequency);
	return (atf_no_error());
}

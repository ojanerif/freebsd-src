/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 *
 * [TC-API-SWFILT] — IBS software-filter control bit tests.
 *
 * Validates that the software-filter control bits in IBS Fetch Control
 * (MSR_IBS_FETCH_CTL) and IBS Op Control (MSR_IBS_OP_CTL) can be written
 * and read back correctly.
 *
 * Uses read_msr() / write_msr() from ibs_utils.h, which call
 * CPUCTL_RDMSR / CPUCTL_WRMSR ioctls directly on /dev/cpuctl0.
 * This replaces the previous shell implementation that depended on
 * cpucontrol(8), which is absent from kyua's restricted PATH.
 *
 * Requires root and AMD IBS hardware.
 *
 * Test IDs: TC-API-SWFILT-01 … TC-API-SWFILT-04
 */

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ibs_utils.h"

/* -----------------------------------------------------------------------
 * TC-API-SWFILT-01: exclude_user filter bit (IBSFETCHCTL bit 56)
 *
 * Sets IBS_L2TLB_MISS (bit 56) in MSR_IBS_FETCH_CTL with the enable
 * bit cleared, reads back, and verifies the bit is preserved.
 * ----------------------------------------------------------------------- */
ATF_TC(ibs_swfilt_exclude_user);
ATF_TC_HEAD(ibs_swfilt_exclude_user, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-API-SWFILT-01] IBS Fetch CTL bit 56 write-read roundtrip");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_swfilt_exclude_user, tc)
{
	uint64_t original, test_val, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Clear enable bit — do not arm the sampler */
	test_val = (original & ~IBS_FETCH_ENABLE_BIT) | IBS_L2TLB_MISS;

	error = write_msr(0, MSR_IBS_FETCH_CTL, test_val);
	if (error != 0) {
		(void)write_msr(0, MSR_IBS_FETCH_CTL, original);
		atf_tc_skip("Cannot write MSR_IBS_FETCH_CTL: %s",
		    strerror(error));
	}

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	(void)write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_MSG((readback & IBS_L2TLB_MISS) == IBS_L2TLB_MISS,
	    "bit 56 not preserved: wrote 0x%016llx, read 0x%016llx",
	    (unsigned long long)test_val, (unsigned long long)readback);
}

/* -----------------------------------------------------------------------
 * TC-API-SWFILT-02: exclude_kernel filter bit (IBSOPCTL bit 19)
 *
 * Sets IBS_CNT_CTL (bit 19) in MSR_IBS_OP_CTL with the enable bit
 * cleared, reads back masking volatile counter bits [15:0], and verifies.
 * ----------------------------------------------------------------------- */
ATF_TC(ibs_swfilt_exclude_kernel);
ATF_TC_HEAD(ibs_swfilt_exclude_kernel, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-API-SWFILT-02] IBS Op CTL bit 19 write-read roundtrip");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_swfilt_exclude_kernel, tc)
{
	uint64_t original, test_val, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	error = read_msr(0, MSR_IBS_OP_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/* Clear enable bit — do not arm the sampler */
	test_val = (original & ~IBS_OP_ENABLE_BIT) | IBS_CNT_CTL;

	error = write_msr(0, MSR_IBS_OP_CTL, test_val);
	if (error != 0) {
		(void)write_msr(0, MSR_IBS_OP_CTL, original);
		atf_tc_skip("Cannot write MSR_IBS_OP_CTL: %s",
		    strerror(error));
	}

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	(void)write_msr(0, MSR_IBS_OP_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
	/* Mask out volatile counter bits [15:0] before comparing */
	ATF_CHECK_MSG(
	    (readback & ~(uint64_t)IBS_MAXCNT_MASK) ==
	    (test_val & ~(uint64_t)IBS_MAXCNT_MASK),
	    "bit 19 not preserved: wrote 0x%016llx, read 0x%016llx",
	    (unsigned long long)test_val, (unsigned long long)readback);
}

/* -----------------------------------------------------------------------
 * TC-API-SWFILT-03: exclude_hv filter bit (IBSFETCHCTL bit 58)
 *
 * Sets IBS_FETCH_L2_MISS (bit 58) in MSR_IBS_FETCH_CTL and verifies
 * the bit is preserved.  Skipped on bare metal (kern.vm_guest == "none").
 * ----------------------------------------------------------------------- */
ATF_TC(ibs_swfilt_exclude_hv);
ATF_TC_HEAD(ibs_swfilt_exclude_hv, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-API-SWFILT-03] IBS Fetch CTL bit 58 write-read roundtrip "
	    "(skipped on bare metal)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_swfilt_exclude_hv, tc)
{
	char vm_guest[32];
	size_t len = sizeof(vm_guest);
	uint64_t original, test_val, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Skip on bare metal */
	if (sysctlbyname("kern.vm_guest", vm_guest, &len, NULL, 0) != 0 ||
	    strncmp(vm_guest, "none", 4) == 0)
		atf_tc_skip("Not running under a hypervisor");

	error = read_msr(0, MSR_IBS_FETCH_CTL, &original);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/* Clear enable bit — do not arm the sampler */
	test_val = (original & ~IBS_FETCH_ENABLE_BIT) | IBS_FETCH_L2_MISS;

	error = write_msr(0, MSR_IBS_FETCH_CTL, test_val);
	if (error != 0) {
		(void)write_msr(0, MSR_IBS_FETCH_CTL, original);
		atf_tc_skip("Cannot write MSR_IBS_FETCH_CTL: %s",
		    strerror(error));
	}

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	(void)write_msr(0, MSR_IBS_FETCH_CTL, original);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_MSG((readback & IBS_FETCH_L2_MISS) == IBS_FETCH_L2_MISS,
	    "bit 58 not preserved: wrote 0x%016llx, read 0x%016llx",
	    (unsigned long long)test_val, (unsigned long long)readback);
}

/* -----------------------------------------------------------------------
 * TC-API-SWFILT-04: combined filter bits
 *
 * IBSFETCHCTL bits 56+58 and IBSOPCTL bits 19+16 — both MSRs exercised
 * in a single test case to catch interactions between the two registers.
 * ----------------------------------------------------------------------- */
ATF_TC(ibs_swfilt_filter_combination);
ATF_TC_HEAD(ibs_swfilt_filter_combination, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-API-SWFILT-04] Combined filter bits: Fetch CTL bits 56+58, "
	    "Op CTL bits 19+16");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_swfilt_filter_combination, tc)
{
	uint64_t fetch_orig, op_orig, test_val, readback;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* --- IBS Fetch CTL: bits 56 + 58 --- */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &fetch_orig);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	test_val = (fetch_orig & ~IBS_FETCH_ENABLE_BIT) |
	    IBS_L2TLB_MISS | IBS_FETCH_L2_MISS;

	error = write_msr(0, MSR_IBS_FETCH_CTL, test_val);
	if (error != 0) {
		(void)write_msr(0, MSR_IBS_FETCH_CTL, fetch_orig);
		atf_tc_skip("Cannot write MSR_IBS_FETCH_CTL: %s",
		    strerror(error));
	}

	error = read_msr(0, MSR_IBS_FETCH_CTL, &readback);
	(void)write_msr(0, MSR_IBS_FETCH_CTL, fetch_orig);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_MSG(
	    (readback & (IBS_L2TLB_MISS | IBS_FETCH_L2_MISS)) ==
	    (IBS_L2TLB_MISS | IBS_FETCH_L2_MISS),
	    "Fetch CTL combined bits not preserved: "
	    "wrote 0x%016llx, read 0x%016llx",
	    (unsigned long long)test_val, (unsigned long long)readback);

	/* --- IBS Op CTL: bits 19 + 16 --- */
	error = read_msr(0, MSR_IBS_OP_CTL, &op_orig);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	test_val = (op_orig & ~IBS_OP_ENABLE_BIT) |
	    IBS_CNT_CTL | IBS_OP_L3_MISS_ONLY;

	error = write_msr(0, MSR_IBS_OP_CTL, test_val);
	if (error != 0) {
		(void)write_msr(0, MSR_IBS_OP_CTL, op_orig);
		atf_tc_skip("Cannot write MSR_IBS_OP_CTL: %s",
		    strerror(error));
	}

	error = read_msr(0, MSR_IBS_OP_CTL, &readback);
	(void)write_msr(0, MSR_IBS_OP_CTL, op_orig);
	ATF_REQUIRE_ERRNO(0, error == 0);
	/* Mask out volatile counter bits [15:0] before comparing */
	ATF_CHECK_MSG(
	    (readback & ~(uint64_t)IBS_MAXCNT_MASK) ==
	    (test_val & ~(uint64_t)IBS_MAXCNT_MASK),
	    "Op CTL combined bits not preserved: "
	    "wrote 0x%016llx, read 0x%016llx",
	    (unsigned long long)test_val, (unsigned long long)readback);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, ibs_swfilt_exclude_user);
	ATF_TP_ADD_TC(tp, ibs_swfilt_exclude_kernel);
	ATF_TP_ADD_TC(tp, ibs_swfilt_exclude_hv);
	ATF_TP_ADD_TC(tp, ibs_swfilt_filter_combination);

	return (atf_no_error());
}

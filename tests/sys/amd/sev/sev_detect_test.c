/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 *
 * [TC-DET-SEV] AMD Secure Encrypted Virtualization detection tests.
 *
 * These tests probe hardware capability and active status via CPUID and MSR
 * reads only.  No encryption keys are allocated, no memory is mapped, and no
 * SEV-specific firmware calls are made.  All four cases are expected to pass
 * on Naples (Zen 1) and later AMD EPYC/Ryzen hardware; earlier or non-AMD
 * CPUs skip with an explanatory message.
 *
 * Hardware interface:
 *   CPUID 0x8000001F  — AMD Memory Encryption capabilities leaf
 *   MSR   0xC0010131  — SEV_STATUS: reports active SEV/SEV-ES/SEV-SNP state
 *
 * Access mechanism: cpuctl(4) via /dev/cpuctl0 (CPUCTL_CPUID, CPUCTL_RDMSR).
 * Requires root.  Load cpuctl.ko if /dev/cpuctl0 is absent.
 *
 * Jira: SWLSVROS-6594
 */

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * CPUID leaf 0x8000001F — AMD Memory Encryption Information
 * Not yet defined in FreeBSD specialreg.h; local definitions used here.
 * Reference: AMD64 APM Vol. 2, Section 15.34 / AMD PPR Family 19h.
 * ---------------------------------------------------------------------- */
#define CPUID_AMD_SEV_INFO		0x8000001fU

/* EAX bits — feature support flags */
#define AMDID_SME			(1U << 0)  /* Secure Memory Encryption */
#define AMDID_SEV			(1U << 1)  /* Secure Encrypted Virtualization */
#define AMDID_PAGE_FLUSH_MSR		(1U << 2)  /* Page Flush MSR supported */
#define AMDID_SEV_ES			(1U << 3)  /* SEV Encrypted State */
#define AMDID_SEV_SNP			(1U << 4)  /* SEV Secure Nested Paging */
#define AMDID_VMPL			(1U << 5)  /* VM Permission Levels */

/* EBX fields */
#define AMDID_SEV_CBITPOS_MASK		0x3fU       /* bits[5:0]: C-bit position */
#define AMDID_SEV_REDUCED_PHYS_MASK	0x3fU       /* bits[11:6]: addr reduction */
#define AMDID_SEV_REDUCED_PHYS_SHIFT	6

/* C-bit position valid range on current EPYC generations */
#define AMDID_SEV_CBITPOS_MIN		47U
#define AMDID_SEV_CBITPOS_MAX		51U

/* -------------------------------------------------------------------------
 * MSR 0xC0010131 — MSR_AMD64_SEV (SEV_STATUS)
 * Non-interceptable inside a guest; reflects hardware-enforced active state.
 * Reference: AMD64 APM Vol. 2, Section 15.34.10.
 * ---------------------------------------------------------------------- */
#define MSR_AMD64_SEV			0xC0010131U
#define MSR_AMD64_SEV_ENABLED		(1ULL << 0)  /* SEV active */
#define MSR_AMD64_SEV_ES_ENABLED	(1ULL << 1)  /* SEV-ES active */
#define MSR_AMD64_SEV_SNP_ENABLED	(1ULL << 2)  /* SEV-SNP active */

/* -------------------------------------------------------------------------
 * AMD vendor string components (CPUID leaf 0x0, EBX/ECX/EDX)
 * "AuthenticAMD"
 * ---------------------------------------------------------------------- */
#define AMD_VENDOR_EBX			0x68747541U  /* "Auth" */
#define AMD_VENDOR_ECX			0x444d4163U  /* "cAMD" */
#define AMD_VENDOR_EDX			0x69746e65U  /* "enti" */

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int
sev_do_cpuid(uint32_t level, uint32_t regs[4])
{
	cpuctl_cpuid_args_t args;
	int error, fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (errno);

	memset(&args, 0, sizeof(args));
	args.level = level;
	if (ioctl(fd, CPUCTL_CPUID, &args) < 0) {
		error = errno;
		(void)close(fd);
		return (error);
	}

	regs[0] = args.data[0];
	regs[1] = args.data[1];
	regs[2] = args.data[2];
	regs[3] = args.data[3];
	(void)close(fd);
	return (0);
}

static int
sev_read_msr(uint32_t msr, uint64_t *val)
{
	cpuctl_msr_args_t args;
	int error, fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (errno);

	memset(&args, 0, sizeof(args));
	args.msr = msr;
	if (ioctl(fd, CPUCTL_RDMSR, &args) < 0) {
		error = errno;
		(void)close(fd);
		return (error);
	}

	*val = args.data;
	(void)close(fd);
	return (0);
}

/* Returns true if /dev/cpuctl0 is present and the CPU is AuthenticAMD. */
static bool
sev_is_amd(void)
{
	uint32_t regs[4];

	if (sev_do_cpuid(0x0, regs) != 0)
		return (false);
	return (regs[1] == AMD_VENDOR_EBX &&
	    regs[3] == AMD_VENDOR_EDX &&
	    regs[2] == AMD_VENDOR_ECX);
}

/* Returns true if CPUID max extended leaf >= 0x8000001F. */
static bool
sev_leaf_reachable(void)
{
	uint32_t regs[4];

	if (sev_do_cpuid(0x80000000U, regs) != 0)
		return (false);
	return (regs[0] >= CPUID_AMD_SEV_INFO);
}

/* -------------------------------------------------------------------------
 * TC-DET-SEV-01  sev_cpuid_leaf_present
 *
 * Verify that CPUID max extended leaf >= 0x8000001F.  This is the
 * prerequisite for all subsequent SEV tests.  Introduced with Naples
 * (Family 17h Model 00h); absent on pre-Naples and non-AMD CPUs.
 * ---------------------------------------------------------------------- */
ATF_TC(sev_cpuid_leaf_present);
ATF_TC_HEAD(sev_cpuid_leaf_present, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify CPUID max extended leaf >= 0x8000001F (AMD SEV info leaf). "
	    "Prerequisite for all SEV detection tests. "
	    "Skips on non-AMD CPUs and pre-Naples AMD hardware.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(sev_cpuid_leaf_present, tc)
{
	uint32_t regs[4];
	int error;

	if (!sev_is_amd())
		atf_tc_skip("CPU is not AuthenticAMD");

	error = sev_do_cpuid(0x80000000U, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID(0x80000000) failed: %s", strerror(error));

	printf("Max extended CPUID leaf: 0x%08x\n", regs[0]);

	ATF_CHECK_MSG(regs[0] >= CPUID_AMD_SEV_INFO,
	    "Max extended leaf 0x%08x < 0x8000001F — "
	    "SEV info leaf not available (pre-Naples CPU?)", regs[0]);
}

/* -------------------------------------------------------------------------
 * TC-DET-SEV-02  sev_capability_probe
 *
 * Read CPUID 0x8000001F and report the full SEV capability set:
 *   EAX — feature flags (SME, SEV, SEV-ES, SEV-SNP, VMPL)
 *   EBX — C-bit position and physical address reduction
 *   ECX — maximum simultaneous encrypted guests
 *   EDX — minimum SEV ASID
 *
 * Asserts at least one of SME or SEV is advertised.  A CPU that reaches
 * leaf 0x8000001F but reports neither is unexpected and likely a firmware
 * defect.
 * ---------------------------------------------------------------------- */
ATF_TC(sev_capability_probe);
ATF_TC_HEAD(sev_capability_probe, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Read CPUID 0x8000001F and report AMD memory encryption capabilities. "
	    "Verifies SME or SEV is advertised on Naples+ hardware. "
	    "Pure CPUID discovery — no MSR access, no hardware state changed.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(sev_capability_probe, tc)
{
	uint32_t regs[4];
	uint32_t cbitpos, reduced_phys;
	int error;

	if (!sev_is_amd())
		atf_tc_skip("CPU is not AuthenticAMD");

	if (!sev_leaf_reachable())
		atf_tc_skip("CPUID leaf 0x8000001F not available on this CPU "
		    "(pre-Naples or too old)");

	error = sev_do_cpuid(CPUID_AMD_SEV_INFO, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID(0x8000001F) failed: %s", strerror(error));

	cbitpos     = regs[1] & AMDID_SEV_CBITPOS_MASK;
	reduced_phys = (regs[1] >> AMDID_SEV_REDUCED_PHYS_SHIFT) &
	    AMDID_SEV_REDUCED_PHYS_MASK;

	printf("CPUID 0x8000001F:\n");
	printf("  EAX=0x%08x  EBX=0x%08x  ECX=0x%08x  EDX=0x%08x\n",
	    regs[0], regs[1], regs[2], regs[3]);
	printf("  SME:     %s\n", (regs[0] & AMDID_SME)     ? "yes" : "no");
	printf("  SEV:     %s\n", (regs[0] & AMDID_SEV)     ? "yes" : "no");
	printf("  SEV-ES:  %s\n", (regs[0] & AMDID_SEV_ES)  ? "yes" : "no");
	printf("  SEV-SNP: %s\n", (regs[0] & AMDID_SEV_SNP) ? "yes" : "no");
	printf("  VMPL:    %s\n", (regs[0] & AMDID_VMPL)    ? "yes" : "no");
	printf("  C-bit position:        %u\n", cbitpos);
	printf("  Physical addr reduction: %u bits\n", reduced_phys);
	printf("  Max encrypted guests:  %u\n", regs[2]);
	printf("  Min SEV ASID:          %u\n", regs[3]);

	ATF_CHECK_MSG((regs[0] & (AMDID_SME | AMDID_SEV)) != 0,
	    "CPUID 0x8000001F EAX=0x%08x: neither SME nor SEV advertised — "
	    "unexpected on Naples+ hardware; possible firmware defect", regs[0]);
}

/* -------------------------------------------------------------------------
 * TC-DET-SEV-03  sev_status_msr_read
 *
 * Read MSR 0xC0010131 (MSR_AMD64_SEV / SEV_STATUS) and report which
 * features are currently active.
 *
 * This MSR is non-interceptable inside a SEV-ES or SEV-SNP guest — the
 * hypervisor cannot forge its value.  On bare metal or a non-SEV VM, all
 * bits read as 0, which is also a valid result.
 *
 * Consistency check: if the MSR reports SEV active (bit 0), CPUID EAX[1]
 * must also be set.  A mismatch indicates a firmware or hypervisor defect.
 * ---------------------------------------------------------------------- */
ATF_TC(sev_status_msr_read);
ATF_TC_HEAD(sev_status_msr_read, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Read MSR 0xC0010131 (SEV_STATUS) via cpuctl(4) and report active "
	    "SEV/SEV-ES/SEV-SNP state.  A value of 0 is valid on bare metal or "
	    "in a non-SEV VM.  Checks consistency against CPUID 0x8000001F EAX.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(sev_status_msr_read, tc)
{
	uint32_t cpuid_regs[4];
	uint64_t sev_status;
	int error;

	if (!sev_is_amd())
		atf_tc_skip("CPU is not AuthenticAMD");

	if (!sev_leaf_reachable())
		atf_tc_skip("CPUID leaf 0x8000001F not available on this CPU");

	error = sev_read_msr(MSR_AMD64_SEV, &sev_status);
	if (error == ENODEV || error == ENXIO)
		atf_tc_skip("MSR 0xC0010131 not accessible — "
		    "load cpuctl.ko or check /dev/cpuctl0 permissions");
	ATF_REQUIRE_MSG(error == 0,
	    "RDMSR(0xC0010131) failed: %s", strerror(error));

	printf("MSR 0xC0010131 (SEV_STATUS): 0x%016llx\n",
	    (unsigned long long)sev_status);
	printf("  SEV active:     %s\n",
	    (sev_status & MSR_AMD64_SEV_ENABLED)     ? "yes" : "no");
	printf("  SEV-ES active:  %s\n",
	    (sev_status & MSR_AMD64_SEV_ES_ENABLED)  ? "yes" : "no");
	printf("  SEV-SNP active: %s\n",
	    (sev_status & MSR_AMD64_SEV_SNP_ENABLED) ? "yes" : "no");

	/*
	 * Consistency check: MSR active implies CPUID capability set.
	 * The reverse is not required — capability present does not mean
	 * the feature is currently active on this VM/host.
	 */
	if (sev_status & MSR_AMD64_SEV_ENABLED) {
		error = sev_do_cpuid(CPUID_AMD_SEV_INFO, cpuid_regs);
		ATF_REQUIRE_MSG(error == 0,
		    "CPUID(0x8000001F) failed during consistency check: %s",
		    strerror(error));
		ATF_CHECK_MSG((cpuid_regs[0] & AMDID_SEV) != 0,
		    "Consistency fail: MSR SEV_STATUS[0]=1 but CPUID "
		    "0x8000001F EAX[1]=0 — firmware or hypervisor defect");
	}
}

/* -------------------------------------------------------------------------
 * TC-DET-SEV-04  sev_cbitpos_valid
 *
 * Verify the C-bit position (CPUID 0x8000001F EBX[5:0]) is within the
 * architecturally valid range for current EPYC generations (47–51).
 *
 * The C-bit is the physical address bit that, when set in a page-table
 * entry, marks that page as encrypted.  An out-of-range value indicates
 * a firmware defect or an emulated environment returning a synthetic leaf.
 *
 * Skips if SEV is not advertised (C-bit is meaningless without SEV).
 * ---------------------------------------------------------------------- */
ATF_TC(sev_cbitpos_valid);
ATF_TC_HEAD(sev_cbitpos_valid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify CPUID 0x8000001F EBX[5:0] C-bit position is in the valid "
	    "range 47-51 for current AMD EPYC generations. "
	    "Skips if SEV is not advertised in EAX.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(sev_cbitpos_valid, tc)
{
	uint32_t regs[4];
	uint32_t cbitpos;
	int error;

	if (!sev_is_amd())
		atf_tc_skip("CPU is not AuthenticAMD");

	if (!sev_leaf_reachable())
		atf_tc_skip("CPUID leaf 0x8000001F not available on this CPU");

	error = sev_do_cpuid(CPUID_AMD_SEV_INFO, regs);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID(0x8000001F) failed: %s", strerror(error));

	if ((regs[0] & AMDID_SEV) == 0)
		atf_tc_skip("CPU does not advertise SEV — "
		    "C-bit position is not meaningful");

	cbitpos = regs[1] & AMDID_SEV_CBITPOS_MASK;
	printf("C-bit position: %u (EBX=0x%08x)\n", cbitpos, regs[1]);

	ATF_CHECK_MSG(cbitpos >= AMDID_SEV_CBITPOS_MIN &&
	    cbitpos <= AMDID_SEV_CBITPOS_MAX,
	    "C-bit position %u is outside expected range [%u, %u] for "
	    "current EPYC generations — possible firmware defect or "
	    "synthetic CPUID from emulator",
	    cbitpos, AMDID_SEV_CBITPOS_MIN, AMDID_SEV_CBITPOS_MAX);
}

/* -------------------------------------------------------------------------
 * Test program registration
 * ---------------------------------------------------------------------- */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, sev_cpuid_leaf_present);
	ATF_TP_ADD_TC(tp, sev_capability_probe);
	ATF_TP_ADD_TC(tp, sev_status_msr_read);
	ATF_TP_ADD_TC(tp, sev_cbitpos_valid);
	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * AMD Secure Encrypted Virtualization (SEV) detection and status module.
 *
 * This module provides detection for AMD SEV, SEV-ES (Encrypted State),
 * and SEV-SNP (Secure Nested Paging) features. It exposes status
 * information via the hw.amd.sev sysctl tree.
 *
 * References:
 * - AMD APM Volume 2: System Programming (Publication 24594)
 * - AMD SEV API Specification (Publication 56860)
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>
#include <x86/cpuinfo.h>

/* SEV feature status variables */
static int sev_supported;
static int sev_es_supported;
static int sev_snp_supported;
static int sev_enabled;
static int sev_es_enabled;
static int sev_snp_enabled;
static uint64_t sev_msr_value;

/* CPUID leaf for AMD SEV features */
#define AMD_CPUID_SEV_LEAF	0x8000001f

/* CPUID feature bits for SEV */
#define CPUID_SEV_BIT		0	/* SEV supported */
#define CPUID_SEV_ES_BIT	1	/* SEV-ES supported */
#define CPUID_SEV_SNP_BIT	4	/* SEV-SNP supported */
#define CPUID_SEV_PAGE_FLUSH_BIT 5	/* Page flush MSR supported */
#define CPUID_SEV_RMPQUERY_BIT	18	/* RMPQUERY instruction supported */

/* Sysctl tree */
SYSCTL_NODE(_hw_amd, OID_AUTO, sev, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
    "AMD SEV (Secure Encrypted Virtualization) status");

SYSCTL_INT(_hw_amd_sev, OID_AUTO, supported, CTLFLAG_RD,
    &sev_supported, 0, "SEV feature is supported by CPU");
SYSCTL_INT(_hw_amd_sev, OID_AUTO, es_supported, CTLFLAG_RD,
    &sev_es_supported, 0, "SEV-ES (Encrypted State) feature is supported");
SYSCTL_INT(_hw_amd_sev, OID_AUTO, snp_supported, CTLFLAG_RD,
    &sev_snp_supported, 0, "SEV-SNP (Secure Nested Paging) is supported");

SYSCTL_INT(_hw_amd_sev, OID_AUTO, enabled, CTLFLAG_RD,
    &sev_enabled, 0, "SEV is currently enabled");
SYSCTL_INT(_hw_amd_sev, OID_AUTO, es_enabled, CTLFLAG_RD,
    &sev_es_enabled, 0, "SEV-ES is currently enabled");
SYSCTL_INT(_hw_amd_sev, OID_AUTO, snp_enabled, CTLFLAG_RD,
    &sev_snp_enabled, 0, "SEV-SNP is currently enabled");

SYSCTL_UQUAD(_hw_amd_sev, OID_AUTO, msr_value, CTLFLAG_RD,
    &sev_msr_value, 0, "Raw value of MSR_AMD64_SEV");

/*
 * Detect AMD SEV features via CPUID and MSR reads.
 *
 * CPUID leaf 0x8000001f (EAX) contains SEV feature bits:
 *   Bit 0:  SEV supported
 *   Bit 1:  SEV-ES supported
 *   Bit 4:  SEV-SNP supported
 *   Bit 5:  Page flush MSR supported
 *   Bit 18: RMPQUERY instruction supported
 *
 * MSR 0xc0010131 (MSR_AMD64_SEV) contains enable status:
 *   Bit 0:  SEV enabled
 *   Bit 1:  SEV-ES enabled
 *   Bit 2:  SEV-SNP enabled
 */
static void
amd_sev_detect(void)
{
	uint64_t msr_val;
	uint32_t eax, ebx, ecx, edx;

	/* Check if CPU is AMD */
	if (cpu_vendor_id != CPU_VENDOR_AMD) {
		sev_supported = 0;
		sev_es_supported = 0;
		sev_snp_supported = 0;
		return;
	}

	/* Check for CPUID leaf 0x8000001f */
	do_cpuid(AMD_CPUID_SEV_LEAF, eax, ebx, ecx, edx);

	/* Check SEV support (bit 0 of EAX) */
	sev_supported = (eax & (1U << CPUID_SEV_BIT)) != 0;

	/* Check SEV-ES support (bit 1 of EAX) */
	sev_es_supported = (eax & (1U << CPUID_SEV_ES_BIT)) != 0;

	/* Check SEV-SNP support (bit 4 of EAX) */
	sev_snp_supported = (eax & (1U << CPUID_SEV_SNP_BIT)) != 0;

	/* If no SEV features are supported, skip MSR read */
	if (!sev_supported && !sev_es_supported && !sev_snp_supported)
		return;

	/* Read the SEV MSR to check enable status */
	msr_val = rdmsr(MSR_AMD64_SEV);
	sev_msr_value = msr_val;

	/* Check if SEV is enabled (bit 0) */
	sev_enabled = (msr_val & MSR_AMD64_SEV_ENABLED) != 0;

	/* Check if SEV-ES is enabled (bit 1) */
	sev_es_enabled = (msr_val & MSR_AMD64_SEV_ES_ENABLED) != 0;

	/* Check if SEV-SNP is enabled (bit 2) */
	sev_snp_enabled = (msr_val & MSR_AMD64_SEV_SNP_ENABLED) != 0;
}

static int
amd_sev_modevent(module_t mod, int type, void *unused)
{

	switch (type) {
	case MOD_LOAD:
		amd_sev_detect();
		break;
	case MOD_UNLOAD:
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t amd_sev_mod = {
	"amd_sev",
	amd_sev_modevent,
	NULL
};

DECLARE_MODULE(amd_sev, amd_sev_mod, SI_SUB_CPU, SI_ORDER_ANY);
MODULE_VERSION(amd_sev, 1);
MODULE_DEPEND(amd_sev, cpuctl, 1, 1, 1);

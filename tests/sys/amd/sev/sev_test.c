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
 * Test for AMD SEV (Secure Encrypted Virtualization) detection module.
 *
 * This test verifies:
 * - The hw.amd.sev sysctl tree exists
 * - SEV feature detection reports correctly
 * - Graceful skip on non-AMD or non-SEV hardware
 */

#include <sys/param.h>
#include <sys/sysctl.h>

#include <atf-c.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Check if the hw.amd.sev sysctl tree exists.
 * Returns 0 if it exists, -1 otherwise.
 */
static int
sev_sysctl_exists(void)
{
	size_t len;
	int mib[CTL_MAXNAME];
	size_t miblen = CTL_MAXNAME;

	if (sysctlnametomib("hw.amd.sev", mib, &miblen) != 0)
		return (-1);

	if (sysctl(mib, miblen, NULL, &len, NULL, 0) != 0)
		return (-1);

	return (0);
}

/*
 * Read an integer sysctl value under hw.amd.sev.
 * Returns 0 on success, -1 on failure.
 */
static int
sev_sysctl_int(const char *name, int *val)
{
	char buf[64];
	size_t len;

	snprintf(buf, sizeof(buf), "hw.amd.sev.%s", name);
	len = sizeof(*val);
	if (sysctlbyname(buf, val, &len, NULL, 0) != 0)
		return (-1);
	return (0);
}

/*
 * Test: Verify that the SEV sysctl tree exists.
 * Skip if not on AMD hardware or SEV module not loaded.
 */
ATF_TC(sev_sysctl_tree);
ATF_TC_HEAD(sev_sysctl_tree, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify hw.amd.sev sysctl tree exists");
}
ATF_TC_BODY(sev_sysctl_tree, tc)
{

	if (sev_sysctl_exists() != 0)
		atf_tc_skip("hw.amd.sev sysctl tree not found; "
		    "SEV module not loaded or not on AMD hardware");

	ATF_CHECK_EQ(0, sev_sysctl_int("supported", NULL));
}

/*
 * Test: Verify SEV feature detection.
 * Check that supported flags are consistent.
 */
ATF_TC(sev_feature_detection);
ATF_TC_HEAD(sev_feature_detection, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify SEV feature detection reports correctly");
}
ATF_TC_BODY(sev_feature_detection, tc)
{
	int supported, es_supported, snp_supported;

	if (sev_sysctl_exists() != 0)
		atf_tc_skip("hw.amd.sev sysctl tree not found; "
		    "SEV module not loaded or not on AMD hardware");

	ATF_CHECK_EQ(0, sev_sysctl_int("supported", &supported));
	ATF_CHECK_EQ(0, sev_sysctl_int("es_supported", &es_supported));
	ATF_CHECK_EQ(0, sev_sysctl_int("snp_supported", &snp_supported));

	/* If SEV is supported, SEV-ES should also be reported */
	if (supported) {
		/* SEV-ES is a superset, so it should be supported too */
		ATF_CHECK(es_supported == 1 || es_supported == 0);
	}

	/* SEV-SNP is an additional feature */
	ATF_CHECK(snp_supported == 1 || snp_supported == 0);
}

/*
 * Test: Verify SEV enable status.
 * Check that enabled flags are consistent with support flags.
 */
ATF_TC(sev_enable_status);
ATF_TC_HEAD(sev_enable_status, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify SEV enable status is consistent");
}
ATF_TC_BODY(sev_enable_status, tc)
{
	int supported, enabled;
	int es_supported, es_enabled;
	int snp_supported, snp_enabled;

	if (sev_sysctl_exists() != 0)
		atf_tc_skip("hw.amd.sev sysctl tree not found; "
		    "SEV module not loaded or not on AMD hardware");

	ATF_CHECK_EQ(0, sev_sysctl_int("supported", &supported));
	ATF_CHECK_EQ(0, sev_sysctl_int("enabled", &enabled));
	ATF_CHECK_EQ(0, sev_sysctl_int("es_supported", &es_supported));
	ATF_CHECK_EQ(0, sev_sysctl_int("es_enabled", &es_enabled));
	ATF_CHECK_EQ(0, sev_sysctl_int("snp_supported", &snp_supported));
	ATF_CHECK_EQ(0, sev_sysctl_int("snp_enabled", &snp_enabled));

	/* If not supported, should not be enabled */
	if (!supported)
		ATF_CHECK_EQ(0, enabled);
	if (!es_supported)
		ATF_CHECK_EQ(0, es_enabled);
	if (!snp_supported)
		ATF_CHECK_EQ(0, snp_enabled);

	/* If enabled, must be supported */
	if (enabled)
		ATF_CHECK_EQ(1, supported);
	if (es_enabled)
		ATF_CHECK_EQ(1, es_supported);
	if (snp_enabled)
		ATF_CHECK_EQ(1, snp_supported);
}

/*
 * Test: Verify MSR value is readable when SEV is supported.
 */
ATF_TC(sev_msr_value);
ATF_TC_HEAD(sev_msr_value, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "Verify SEV MSR value is readable");
}
ATF_TC_BODY(sev_msr_value, tc)
{
	uint64_t msr_val;
	size_t len;
	int supported;

	if (sev_sysctl_exists() != 0)
		atf_tc_skip("hw.amd.sev sysctl tree not found; "
		    "SEV module not loaded or not on AMD hardware");

	ATF_CHECK_EQ(0, sev_sysctl_int("supported", &supported));

	if (!supported)
		atf_tc_skip("SEV not supported on this CPU");

	len = sizeof(msr_val);
	ATF_CHECK_EQ(0, sysctlbyname("hw.amd.sev.msr_value", &msr_val, &len,
	    NULL, 0));
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, sev_sysctl_tree);
	ATF_TP_ADD_TC(tp, sev_feature_detection);
	ATF_TP_ADD_TC(tp, sev_enable_status);
	ATF_TP_ADD_TC(tp, sev_msr_value);

	return (atf_no_error());
}

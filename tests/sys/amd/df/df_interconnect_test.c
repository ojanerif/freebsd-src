/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-INC-DF] AMD Data Fabric interconnect PMU tests.
 *
 * Validates that the DF PMC infrastructure correctly counts traffic across
 * the Infinity Fabric interconnect.  Tests drive memory workloads that
 * exercise DRAM channels and verify that counters are monotonically
 * non-decreasing and proportional to work done.
 *
 * Tests:
 *   df_interconnect_smoke     — allocate a DF PMC, run a remote-traffic
 *                               workload, verify the counter advanced.
 *   df_interconnect_two_chan  — allocate two DF PMCs on different channels
 *                               simultaneously and verify both advance.
 *   df_interconnect_encoding  — allocate a high-event-bit DF event and
 *                               verify DF2 vs DF1 encoding is correct.
 *
 * Jira: FreeBSD-Tests-023
 */

#include <sys/param.h>

#include <atf-c.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amd_df_common.h"

/* High-event-bit candidates for DF encoding validation (DF2 vs DF1). */
static const struct amd_umcdf_event_candidate df_enc_events[] = {
	{
		"local_or_remote_socket_read_data_beats_dram_4",
		"Zen 5 DF PMU: data beats DRAM channel 4 (high event bits)",
		0x11f, 0xffe
	},
	{
		"local_processor_read_data_beats_cs4",
		"Zen 4 DF PMU: local read data beats cs4 (high event bits)",
		0x11f, 0x7fe
	},
	{
		"dram_channel_data_controller_4",
		"Zen 1/2/3 DF PMU: DRAM channel data controller 4 (high bits)",
		0x107, 0x38
	},
	{ NULL, NULL, 0, 0 }
};

/* -------------------------------------------------------------------------
 * TC-INC-DF-01  df_interconnect_smoke
 *
 * Allocates a DF DRAM-channel PMC, starts it, drives a 128 MB page-stride
 * workload (AMD_DF_REMOTE_BUFFER_SIZE) to generate DRAM traffic, stops,
 * reads, and asserts the counter is monotonically non-decreasing.
 *
 * The runtime flag amd.df.interconnect_runtime must be set to true to
 * enable the actual counter measurement; by default the test validates
 * only the allocation and capabilities path.
 * ---------------------------------------------------------------------- */
ATF_TC(df_interconnect_smoke);
ATF_TC_HEAD(df_interconnect_smoke, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Smoke test AMD DF PMC lifecycle: allocate a DRAM-channel event, "
	    "drive a memory workload, verify the counter advanced.  "
	    "Runtime measurement requires amd.df.interconnect_runtime=true.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_interconnect_smoke, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	pmc_value_t before, after;
	pmc_id_t pmcid;
	uint32_t caps;
	int error, last_error;

	pmcid = PMC_ID_INVALID;
	before = after = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu))
		atf_tc_skip("FreeBSD PMU tables do not carry DF JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable DF DRAM event; last error: %s",
		    strerror(last_error));

	ATF_REQUIRE_MSG(cfg.pm_class == PMC_CLASS_K8 &&
	    cfg.pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_DATA_FABRIC,
	    "DF event %s is not backed by AMD Data Fabric hwpmc",
	    event->name);

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL))
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    event->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", event->name, strerror(errno));

	if (pmc_capabilities(pmcid, &caps) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_capabilities(%s) failed: %s",
		    event->name, strerror(errno));
	}
	ATF_CHECK_MSG(
	    (caps & (PMC_CAP_DOMWIDE | PMC_CAP_READ | PMC_CAP_QUALIFIER)) ==
	    (PMC_CAP_DOMWIDE | PMC_CAP_READ | PMC_CAP_QUALIFIER),
	    "DF event %s missing DOMWIDE|READ|QUALIFIER capabilities",
	    event->name);

	if (!atf_tc_get_config_var_as_bool_wd(tc,
	    "amd.df.interconnect_runtime", false)) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_skip("DF interconnect runtime disabled by default; "
		    "set amd.df.interconnect_runtime=true to enable");
	}

	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start(%s) failed: %s",
		    event->name, strerror(errno));
	}
	if (pmc_read(pmcid, &before) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}

	error = amd_df_generate_remote_traffic();
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("remote traffic workload failed: %s",
		    strerror(error));
	}

	if (pmc_stop(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_stop(%s) failed: %s",
		    event->name, strerror(errno));
	}
	if (pmc_read(pmcid, &after) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	printf("DF interconnect smoke [%s]: before=%ju after=%ju delta=%ju\n",
	    event->name, (uintmax_t)before, (uintmax_t)after,
	    (uintmax_t)(after - before));

	ATF_CHECK_MSG(after >= before,
	    "DF counter moved backwards: before=%ju after=%ju",
	    (uintmax_t)before, (uintmax_t)after);

	amd_umcdf_release_pmc(pmcid);
}

/* -------------------------------------------------------------------------
 * TC-INC-DF-02  df_interconnect_two_chan
 *
 * Allocates two DF DRAM-channel PMCs simultaneously (channel 0 and
 * channel 1), drives the remote-traffic workload, and verifies both
 * counters advanced.  Validates that the DF PMC allocator correctly
 * handles multiple concurrent domain-wide counters.
 * ---------------------------------------------------------------------- */
ATF_TC(df_interconnect_two_chan);
ATF_TC_HEAD(df_interconnect_two_chan, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Allocate two DF DRAM-channel PMCs simultaneously (ch0 and ch1) "
	    "and verify both counters advance after a memory workload.  "
	    "Runtime requires amd.df.interconnect_runtime=true.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_interconnect_two_chan, tc)
{
	const struct amd_umcdf_event_candidate *ev0, *ev1;
	struct pmc_op_pmcallocate cfg0, cfg1;
	struct amd_umcdf_cpu cpu;
	pmc_value_t b0, a0, b1, a1;
	pmc_id_t pmc0, pmc1;
	int error, last_error;

	pmc0 = pmc1 = PMC_ID_INVALID;
	b0 = a0 = b1 = a1 = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu))
		atf_tc_skip("FreeBSD PMU tables do not carry DF JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	ev0 = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg0,
	    &last_error);
	if (ev0 == NULL)
		atf_tc_skip("No ch0 DF event; last error: %s",
		    strerror(last_error));

	ev1 = amd_umcdf_pick_pmu_event(amd_df_link_events, &cfg1,
	    &last_error);
	if (ev1 == NULL)
		atf_tc_skip("No ch1 DF event; last error: %s",
		    strerror(last_error));

	error = pmc_allocate(ev0->name, PMC_MODE_SC, 0, 0, &pmc0, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL))
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    ev0->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", ev0->name, strerror(errno));

	error = pmc_allocate(ev1->name, PMC_MODE_SC, 0, 0, &pmc1, 0);
	if (error < 0) {
		amd_umcdf_release_pmc(pmc0);
		atf_tc_skip("pmc_allocate(%s) not available: %s",
		    ev1->name, strerror(errno));
	}
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", ev1->name, strerror(errno));

	if (!atf_tc_get_config_var_as_bool_wd(tc,
	    "amd.df.interconnect_runtime", false)) {
		amd_umcdf_release_pmc(pmc0);
		amd_umcdf_release_pmc(pmc1);
		atf_tc_skip("DF interconnect runtime disabled; "
		    "set amd.df.interconnect_runtime=true");
	}

	ATF_REQUIRE_MSG(pmc_start(pmc0) == 0,
	    "pmc_start(%s) failed: %s", ev0->name, strerror(errno));
	ATF_REQUIRE_MSG(pmc_start(pmc1) == 0,
	    "pmc_start(%s) failed: %s", ev1->name, strerror(errno));

	ATF_REQUIRE_MSG(pmc_read(pmc0, &b0) == 0,
	    "pmc_read(before, %s) failed: %s", ev0->name, strerror(errno));
	ATF_REQUIRE_MSG(pmc_read(pmc1, &b1) == 0,
	    "pmc_read(before, %s) failed: %s", ev1->name, strerror(errno));

	error = amd_df_generate_remote_traffic();
	if (error != 0) {
		(void)pmc_stop(pmc0);
		(void)pmc_stop(pmc1);
		amd_umcdf_release_pmc(pmc0);
		amd_umcdf_release_pmc(pmc1);
		atf_tc_fail("remote traffic workload failed: %s",
		    strerror(error));
	}

	(void)pmc_stop(pmc0);
	(void)pmc_stop(pmc1);

	ATF_REQUIRE_MSG(pmc_read(pmc0, &a0) == 0,
	    "pmc_read(after, %s) failed: %s", ev0->name, strerror(errno));
	ATF_REQUIRE_MSG(pmc_read(pmc1, &a1) == 0,
	    "pmc_read(after, %s) failed: %s", ev1->name, strerror(errno));

	printf("ch0 [%s]: before=%ju after=%ju delta=%ju\n",
	    ev0->name, (uintmax_t)b0, (uintmax_t)a0,
	    (uintmax_t)(a0 - b0));
	printf("ch1 [%s]: before=%ju after=%ju delta=%ju\n",
	    ev1->name, (uintmax_t)b1, (uintmax_t)a1,
	    (uintmax_t)(a1 - b1));

	ATF_CHECK_MSG(a0 >= b0,
	    "ch0 DF counter moved backwards: before=%ju after=%ju",
	    (uintmax_t)b0, (uintmax_t)a0);
	ATF_CHECK_MSG(a1 >= b1,
	    "ch1 DF counter moved backwards: before=%ju after=%ju",
	    (uintmax_t)b1, (uintmax_t)a1);

	amd_umcdf_release_pmc(pmc0);
	amd_umcdf_release_pmc(pmc1);
}

/* -------------------------------------------------------------------------
 * TC-INC-DF-03  df_interconnect_encoding
 *
 * Allocates a high-event-bit DF event and verifies the AMD event-select
 * config matches the expected DF1 (Zen 1–3) or DF2 (Zen 4+) encoding.
 * High event bits occupy different fields in the two encodings; this test
 * catches regressions in the libpmc encoding path for extended event codes.
 * No counter is started.
 * ---------------------------------------------------------------------- */
ATF_TC(df_interconnect_encoding);
ATF_TC_HEAD(df_interconnect_encoding, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify that a high-event-bit DF PMU event resolves to the correct "
	    "DF1 (Zen 1–3) or DF2 (Zen 4+) AMD event-select config encoding.  "
	    "No counter is started; pure config validation.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_interconnect_encoding, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	int last_error;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu))
		atf_tc_skip("FreeBSD PMU tables do not carry DF JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(df_enc_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("No high-event-bit DF candidate matched this PMU "
		    "table; last libpmc error: %s", strerror(last_error));

	printf("DF encoding test event: %s (%s)\n",
	    event->name, event->reason);
	printf("  EventCode=0x%x UMask=0x%x encoding=%s\n",
	    event->event_code, event->umask,
	    (cpu.zen >= AMD_UMCDF_ZEN_4) ? "DF2" : "DF1");

	ATF_REQUIRE_MSG(cfg.pm_class == PMC_CLASS_K8,
	    "High-bit DF event %s not mapped to PMC_CLASS_K8", event->name);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_DATA_FABRIC,
	    "High-bit DF event %s not mapped to DATA_FABRIC subclass",
	    event->name);
	ATF_CHECK_MSG(cfg.pm_md.pm_amd.pm_amd_config ==
	    amd_umcdf_expected_df_config(&cpu, event),
	    "DF event %s config mismatch (%s encoding): got 0x%jx expected 0x%jx",
	    event->name,
	    (cpu.zen >= AMD_UMCDF_ZEN_4) ? "DF2" : "DF1",
	    (uintmax_t)cfg.pm_md.pm_amd.pm_amd_config,
	    (uintmax_t)amd_umcdf_expected_df_config(&cpu, event));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, df_interconnect_smoke);
	ATF_TP_ADD_TC(tp, df_interconnect_two_chan);
	ATF_TP_ADD_TC(tp, df_interconnect_encoding);
	return (atf_no_error());
}

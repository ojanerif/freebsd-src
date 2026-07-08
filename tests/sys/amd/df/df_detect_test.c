/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * [TC-DET-DF] AMD Data Fabric PMU detection tests.
 *
 * These tests probe hardware capability and FreeBSD kernel state without
 * programming any counters.  All cases are expected to pass on Naples
 * (Zen 1) and later AMD EPYC hardware with hwpmc loaded; non-AMD CPUs
 * and pre-Zen hardware skip with an explanatory message.
 *
 * Hardware interface:
 *   CPUID Fn80000001 ECX[24]  — PNXC: Data Fabric PMC availability
 *   CPUID Fn80000022          — PerfMonV2: dynamic DF PMC count (Zen 5+)
 *   hwpmc K8-DF rows          — kernel-side DF event table
 *
 * Access mechanism: cpuctl(4) + libpmc via amd_umcdf_common.h.
 * Requires root.  Load hwpmc.ko if pmc_init() fails.
 *
 * Jira: FreeBSD-Tests-023
 */

#include <sys/param.h>

#include <atf-c.h>

#include "amd_df_common.h"

/* -------------------------------------------------------------------------
 * TC-DET-DF-01  df_capability_probe
 *
 * Reads the AMD vendor string, decodes the Zen generation from CPUID FMS,
 * and checks CPUID Fn80000001 ECX[24] (PNXC) for DF PMC availability.
 * If PerfMonV2 (Fn80000022) is present, reads and prints the dynamic DF
 * PMC count.  No hardware is programmed; pure discovery.
 * ---------------------------------------------------------------------- */
ATF_TC(df_capability_probe);
ATF_TC_HEAD(df_capability_probe, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Probe AMD Data Fabric PMU capability without programming any "
	    "counter.  Reads CPUID Fn80000001 ECX[24] (PNXC) and, if "
	    "available, CPUID Fn80000022 (PerfMonV2) for the dynamic DF PMC "
	    "count.  Skips on non-AMD and pre-Zen CPUs.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_capability_probe, tc)
{
	struct amd_umcdf_perfmon_v2 pmv2;
	struct amd_umcdf_cpu cpu;
	int error;

	amd_umcdf_skip_unless_known_zen(&cpu);

	printf("DF capability probe: %s (family 0x%02x model 0x%02x "
	    "stepping 0x%x)\n",
	    amd_umcdf_zen_name(cpu.zen), cpu.family, cpu.model, cpu.stepping);
	printf("  CPUID Fn80000001 ECX[24] PNXC (DF PMC): %s\n",
	    amd_umcdf_has_df_feature(&cpu) ? "yes" : "no");

	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID Fn80000001 ECX[24] PNXC not set — "
		    "Data Fabric PMCs not advertised on %s",
		    amd_umcdf_zen_name(cpu.zen));

	if (!amd_df_has_freebsd_df_json(&cpu))
		atf_tc_skip("FreeBSD DF PMU JSON not available for %s",
		    amd_umcdf_zen_name(cpu.zen));

	error = amd_umcdf_read_perfmon_v2(&cpu, &pmv2);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID Fn80000022 read failed: %s", strerror(error));

	if (pmv2.leaf_available) {
		printf("PerfMonV2 (Fn80000022): present=%s core_pmcs=%u "
		    "df_pmcs=%u active_umc_mask=0x%x\n",
		    pmv2.present ? "yes" : "no",
		    pmv2.core_pmcs, pmv2.df_pmcs, pmv2.active_umc_mask);

		/*
		 * On Zen 5+ PerfMonV2 must be present.  On older generations
		 * the leaf may be reachable but not marked present.
		 */
		if (cpu.zen >= AMD_UMCDF_ZEN_5)
			ATF_CHECK_MSG(pmv2.present,
			    "Expected PerfMonV2 present on %s "
			    "(CPUID Fn80000022 EAX[0] should be 1)",
			    amd_umcdf_zen_name(cpu.zen));

		if (pmv2.present)
			ATF_CHECK_MSG(pmv2.df_pmcs > 0,
			    "PerfMonV2 reports 0 DF PMCs on %s — "
			    "unexpected when PNXC and PerfMonV2 are both set",
			    amd_umcdf_zen_name(cpu.zen));
	} else {
		printf("PerfMonV2 leaf not available on %s — "
		    "using default DF PMC count (%u)\n",
		    amd_umcdf_zen_name(cpu.zen), AMD_UMCDF_DF_DEFAULT_PMCS);
	}
}

/* -------------------------------------------------------------------------
 * TC-DET-DF-02  df_rows_in_hwpmc
 *
 * Initialises libpmc and counts PMC rows whose name begins with "K8-DF-".
 * Verifies the count matches what CPUID Fn80000022 advertises (Zen 5+) or
 * the FreeBSD default of 4 rows (Zen 1–4).
 * ---------------------------------------------------------------------- */
ATF_TC(df_rows_in_hwpmc);
ATF_TC_HEAD(df_rows_in_hwpmc, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify FreeBSD hwpmc exposes the expected number of K8-DF rows. "
	    "On Zen 5+ the count is cross-checked against CPUID Fn80000022 "
	    "EBX[15:10]; on Zen 1–4 the FreeBSD default of 4 rows is expected.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_rows_in_hwpmc, tc)
{
	struct amd_umcdf_perfmon_v2 pmv2;
	struct amd_umcdf_cpu cpu;
	size_t rows, expected_rows;
	int error;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID Fn80000001 ECX[24] PNXC not set");
	if (!amd_df_has_freebsd_df_json(&cpu))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_hwpmc();

	rows = amd_umcdf_count_pmc_rows_with_prefix(0, "K8-DF-");
	printf("hwpmc K8-DF rows on CPU 0: %zu\n", rows);

	ATF_CHECK_MSG(rows > 0,
	    "Expected at least 1 K8-DF row on %s; hwpmc reported 0 — "
	    "check hwpmc.ko is loaded and pmu-events carries df JSON",
	    amd_umcdf_zen_name(cpu.zen));

	error = amd_umcdf_read_perfmon_v2(&cpu, &pmv2);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID Fn80000022 read failed: %s", strerror(error));

	expected_rows = AMD_UMCDF_DF_DEFAULT_PMCS;
	if (pmv2.leaf_available && pmv2.raw_ebx != 0)
		expected_rows = pmv2.df_pmcs;

	ATF_CHECK_MSG(rows == expected_rows,
	    "hwpmc exposed %zu K8-DF rows, expected %zu for %s",
	    rows, expected_rows, amd_umcdf_zen_name(cpu.zen));
}

/* -------------------------------------------------------------------------
 * TC-DET-DF-03  df_pmu_events_enabled
 *
 * Verifies pmc_pmu_enabled() returns true.  This is a prerequisite for all
 * named DF event lookups.  Fails fast with a useful message when hwpmc is
 * missing or built without PMU-events support.
 * ---------------------------------------------------------------------- */
ATF_TC(df_pmu_events_enabled);
ATF_TC_HEAD(df_pmu_events_enabled, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify pmc_pmu_enabled() returns true — prerequisite for all DF "
	    "named-event lookups (K8-DF-* class).  Skips when hwpmc is absent "
	    "or PMU-events support is not compiled in.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_pmu_events_enabled, tc)
{
	struct amd_umcdf_cpu cpu;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID does not advertise DF PMCs");
	if (!amd_df_has_freebsd_df_json(&cpu))
		atf_tc_skip("No FreeBSD DF PMU JSON for %s",
		    amd_umcdf_zen_name(cpu.zen));

	amd_umcdf_skip_unless_pmu_events();
	printf("PMU named-event support enabled on %s\n",
	    amd_umcdf_zen_name(cpu.zen));
}

/* -------------------------------------------------------------------------
 * TC-DET-DF-04  df_pmu_maps_to_data_fabric
 *
 * Verifies that a generation-appropriate DF PMU event resolves through
 * libpmc to PMC_CLASS_K8 / PMC_AMD_SUB_CLASS_DATA_FABRIC and that the
 * computed AMD event-select config matches the expected DF1/DF2 encoding.
 * No counter is started; pure allocation and config validation.
 * ---------------------------------------------------------------------- */
ATF_TC(df_pmu_maps_to_data_fabric);
ATF_TC_HEAD(df_pmu_maps_to_data_fabric, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify a generation-appropriate DF PMU event maps through libpmc "
	    "to PMC_CLASS_K8 / PMC_AMD_SUB_CLASS_DATA_FABRIC and that the "
	    "AMD event-select config matches the DF1 (Zen 1–3) or DF2 (Zen 4+) "
	    "encoding.  No counter is started.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(df_pmu_maps_to_data_fabric, tc)
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

	event = amd_umcdf_pick_pmu_event(amd_df_dram_events, &cfg,
	    &last_error);
	if (event == NULL)
		atf_tc_skip("No DF PMU event candidate matched this PMU table; "
		    "last libpmc error: %s", strerror(last_error));

	printf("Selected DF event: %s (%s)\n", event->name, event->reason);
	printf("  EventCode=0x%x UMask=0x%x\n",
	    event->event_code, event->umask);

	ATF_REQUIRE_MSG(cfg.pm_class == PMC_CLASS_K8,
	    "DF event %s mapped to class %d, expected PMC_CLASS_K8",
	    event->name, cfg.pm_class);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_DATA_FABRIC,
	    "DF event %s did not map to PMC_AMD_SUB_CLASS_DATA_FABRIC",
	    event->name);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_config != 0,
	    "DF event %s produced an empty AMD event-select config",
	    event->name);
	ATF_CHECK_MSG(cfg.pm_md.pm_amd.pm_amd_config ==
	    amd_umcdf_expected_df_config(&cpu, event),
	    "DF event %s config mismatch: got 0x%jx expected 0x%jx for %s",
	    event->name,
	    (uintmax_t)cfg.pm_md.pm_amd.pm_amd_config,
	    (uintmax_t)amd_umcdf_expected_df_config(&cpu, event),
	    amd_umcdf_zen_name(cpu.zen));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, df_capability_probe);
	ATF_TP_ADD_TC(tp, df_rows_in_hwpmc);
	ATF_TP_ADD_TC(tp, df_pmu_events_enabled);
	ATF_TP_ADD_TC(tp, df_pmu_maps_to_data_fabric);
	return (atf_no_error());
}

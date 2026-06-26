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

static const struct amd_umcdf_event_candidate df_events[] = {
	{
		"local_or_remote_socket_read_data_beats_dram_0",
		"Zen 5 DF PMU data-beat event",
		0x1f, 0xffe
	},
	{
		"local_processor_read_data_beats_cs0",
		"Zen 4 DF PMU read-data event",
		0x1f, 0x7fe
	},
	{
		"dram_channel_data_controller_0",
		"Zen 1/2/3 DF PMU DRAM-channel event",
		0x07, 0x38
	},
	{
		"remote_outbound_data_controller_0",
		"Zen 1/2/3 DF PMU outbound-data event",
		0x7c7, 0x03
	},
	{ NULL, NULL, 0, 0 }
};

static const struct amd_umcdf_event_candidate df_encoding_events[] = {
	{
		"local_or_remote_socket_read_data_beats_dram_4",
		"Zen 5 DF PMU data-beat event with high event bits",
		0x11f, 0xffe
	},
	{
		"local_processor_read_data_beats_cs4",
		"Zen 4 DF PMU read-data event with high event bits",
		0x11f, 0x7fe
	},
	{
		"dram_channel_data_controller_4",
		"Zen 1/2/3 DF PMU DRAM-channel event with high event bits",
		0x107, 0x38
	},
	{ NULL, NULL, 0, 0 }
};

ATF_TC(umcdf_df_rows_match_cpuid);
ATF_TC_HEAD(umcdf_df_rows_match_cpuid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify FreeBSD hwpmc exposes K8-DF rows when AMD CPUID advertises "
	    "Data Fabric PMCs, and compare the row count with Fn80000022 when "
	    "that CPUID leaf is present");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_df_rows_match_cpuid, tc)
{
	struct amd_umcdf_perfmon_v2 pmv2;
	struct amd_umcdf_cpu cpu;
	size_t expected_rows, rows;
	int error;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID Fn80000001 ECX[24] PNXC/Data Fabric PMC "
		    "feature is not set");
	amd_umcdf_skip_unless_hwpmc();

	rows = amd_umcdf_count_pmc_rows_with_prefix(0, "K8-DF-");
	error = amd_umcdf_read_perfmon_v2(&cpu, &pmv2);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID Fn80000022 failed: %s", strerror(error));
	expected_rows = AMD_UMCDF_DF_DEFAULT_PMCS;
	if (pmv2.leaf_available && pmv2.raw_ebx != 0)
		expected_rows = pmv2.df_pmcs;

	ATF_CHECK_MSG(rows == expected_rows,
	    "hwpmc exposed %zu DF rows, expected %zu for %s", rows,
	    expected_rows, amd_umcdf_zen_name(cpu.zen));
	if (pmv2.leaf_available && pmv2.raw_ebx != 0) {
		ATF_CHECK_MSG(rows == (size_t)pmv2.df_pmcs,
		    "hwpmc exposed %zu DF rows, CPUID Fn80000022 EBX[15:10] "
		    "reported %u", rows, pmv2.df_pmcs);
	} else {
		ATF_CHECK_MSG(rows == (size_t)AMD_UMCDF_DF_DEFAULT_PMCS,
		    "hwpmc exposed %zu DF rows without a dynamic CPUID count; "
		    "expected the FreeBSD default %u", rows,
		    AMD_UMCDF_DF_DEFAULT_PMCS);
	}
}

ATF_TC(umcdf_df_pmu_maps_to_data_fabric);
ATF_TC_HEAD(umcdf_df_pmu_maps_to_data_fabric, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify a generation-appropriate DF PMU event maps through libpmc "
	    "to PMC_CLASS_K8 / PMC_AMD_SUB_CLASS_DATA_FABRIC before runtime use");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_df_pmu_maps_to_data_fabric, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	int last_error;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_umcdf_has_freebsd_df_json(&cpu))
		atf_tc_skip("FreeBSD PMU tables in this tree do not carry DF JSON "
		    "for %s", amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(df_encoding_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("No DF PMU encoding candidate matched this PMU table; "
		    "last libpmc error was %s", strerror(last_error));

	printf("Selected DF event: %s (%s)\n", event->name, event->reason);
	ATF_REQUIRE_MSG(cfg.pm_class == PMC_CLASS_K8,
	    "DF PMU event %s mapped to class %d, expected PMC_CLASS_K8",
	    event->name, cfg.pm_class);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_DATA_FABRIC,
	    "DF PMU event %s did not map to DATA_FABRIC subclass", event->name);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_config != 0,
	    "DF PMU event %s produced an empty AMD event-select config",
	    event->name);
	ATF_REQUIRE_MSG(cfg.pm_md.pm_amd.pm_amd_config ==
	    amd_umcdf_expected_df_config(&cpu, event),
	    "DF PMU event %s produced config %#jx, expected %#jx for %s",
	    event->name, (uintmax_t)cfg.pm_md.pm_amd.pm_amd_config,
	    (uintmax_t)amd_umcdf_expected_df_config(&cpu, event),
	    amd_umcdf_zen_name(cpu.zen));
}

ATF_TC(umcdf_df_runtime_smoke);
ATF_TC_HEAD(umcdf_df_runtime_smoke, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Smoke test AMD Data Fabric PMC lifecycle");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_df_runtime_smoke, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	pmc_value_t after, before;
	pmc_id_t pmcid;
	uint32_t caps;
	int error, last_error;

	pmcid = PMC_ID_INVALID;
	before = after = 0;
	caps = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_df_feature(&cpu))
		atf_tc_skip("CPUID does not advertise AMD Data Fabric PMCs");
	if (!amd_umcdf_has_freebsd_df_json(&cpu))
		atf_tc_skip("FreeBSD PMU tables in this tree do not carry DF JSON "
		    "for %s", amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(df_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("No allocatable DF PMU event candidate was found; "
		    "last libpmc error was %s", strerror(last_error));
	ATF_REQUIRE_MSG(cfg.pm_class == PMC_CLASS_K8 &&
	    cfg.pm_md.pm_amd.pm_amd_sub_class ==
	    PMC_AMD_SUB_CLASS_DATA_FABRIC,
	    "DF runtime event %s is not backed by FreeBSD AMD Data Fabric hwpmc",
	    event->name);

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY || errno == EINVAL))
		atf_tc_skip("pmc_allocate(%s) is not available in this test "
		    "environment: %s", event->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed: %s", event->name, strerror(errno));

	if (pmc_capabilities(pmcid, &caps) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_capabilities(%s) failed: %s", event->name,
		    strerror(errno));
	}
	if ((caps & (PMC_CAP_DOMWIDE | PMC_CAP_READ | PMC_CAP_QUALIFIER)) !=
	    (PMC_CAP_DOMWIDE | PMC_CAP_READ | PMC_CAP_QUALIFIER)) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("DF event %s is missing readable domain-wide "
		    "qualifier caps", event->name);
	}
	if (!atf_tc_get_config_var_as_bool_wd(tc, "amd.umcdf.df_runtime",
	    false)) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_skip("DF runtime disabled by default; set "
		    "amd.umcdf.df_runtime=true");
	}

	if (pmc_start(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_start(%s) failed: %s", event->name,
		    strerror(errno));
	}
	if (pmc_read(pmcid, &before) != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(before) failed: %s", strerror(errno));
	}
	error = amd_umcdf_generate_memory_traffic(64 * 1024 * 1024, 4);
	if (error != 0) {
		(void)pmc_stop(pmcid);
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("memory traffic setup failed: %s", strerror(error));
	}
	if (pmc_stop(pmcid) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_stop(%s) failed: %s", event->name,
		    strerror(errno));
	}
	if (pmc_read(pmcid, &after) != 0) {
		amd_umcdf_release_pmc(pmcid);
		atf_tc_fail("pmc_read(after) failed: %s", strerror(errno));
	}

	ATF_CHECK_MSG(after >= before,
	    "DF counter moved backwards: before=%ju after=%ju",
	    (uintmax_t)before, (uintmax_t)after);
	amd_umcdf_release_pmc(pmcid);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, umcdf_df_rows_match_cpuid);
	ATF_TP_ADD_TC(tp, umcdf_df_pmu_maps_to_data_fabric);
	ATF_TP_ADD_TC(tp, umcdf_df_runtime_smoke);
	return (atf_no_error());
}

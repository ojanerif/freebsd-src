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

static const struct amd_umcdf_event_candidate umc_events[] = {
	{
		"umc_cas_cmd.rd",
		"read CAS command count, Unit=UMCPMC, RdWrMask=0x1",
		0, 0
	},
	{
		"umc_cas_cmd.wr",
		"write CAS command count, Unit=UMCPMC, RdWrMask=0x2",
		0, 0
	},
	{
		"umc_data_slot_clks.rd",
		"read data-slot clocks, Unit=UMCPMC, RdWrMask=0x1",
		0, 0
	},
	{
		"umc_mem_clk",
		"UMC memory clock, Unit=UMCPMC",
		0, 0
	},
	{ NULL, NULL, 0, 0 }
};

ATF_TC(umcdf_umc_capability_probe);
ATF_TC_HEAD(umcdf_umc_capability_probe, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Probe Zen 4/5/6 UMC-related CPUID state without programming UMC MSRs. "
	    "This captures the hardware side that must exist before FreeBSD "
	    "runtime UMC support can be validated.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_umc_capability_probe, tc)
{
	struct amd_umcdf_perfmon_v2 pmv2;
	struct amd_umcdf_cpu cpu;
	int error;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_freebsd_umc_json(&cpu))
		atf_tc_skip("FreeBSD currently carries UMC PMU JSON for Zen 4/5/6 "
		    "tables; detected %s", amd_umcdf_zen_name(cpu.zen));

	error = amd_umcdf_read_perfmon_v2(&cpu, &pmv2);
	ATF_REQUIRE_MSG(error == 0,
	    "CPUID Fn80000022 failed: %s", strerror(error));
	if (!pmv2.leaf_available)
		atf_tc_skip("CPUID Fn80000022 is not available; cannot read the "
		    "UMC capability mask path");

	printf("%s UMC probe: PerfMonV2=%s active_umc_mask=%#x df_pmcs=%u\n",
	    amd_umcdf_zen_name(cpu.zen), pmv2.present ? "yes" : "no",
	    pmv2.active_umc_mask, pmv2.df_pmcs);
	ATF_CHECK_MSG(pmv2.present,
	    "Zen 4/5/6 UMC PMU support work should start from PerfMonV2-capable "
	    "hardware");
}

ATF_TC(umcdf_umc_pmu_metadata_contract);
ATF_TC_HEAD(umcdf_umc_pmu_metadata_contract, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify Zen 4/5/6 UMC PMU metadata reaches libpmc.  Current upstream "
	    "FreeBSD should report EOPNOTSUPP because UMCPMC falls through to a "
	    "generic uncore_umcpmc PMU name and the AMD allocator has no amd_umc "
	    "case.  A future UMC backend may make this allocation descriptor valid.");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_umc_pmu_metadata_contract, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	bool saw_enoent, saw_eopnotsupp;
	int error;

	saw_enoent = false;
	saw_eopnotsupp = false;
	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_freebsd_umc_json(&cpu))
		atf_tc_skip("No FreeBSD UMC PMU JSON is expected for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	for (event = umc_events; event->name != NULL; event++) {
		memset(&cfg, 0, sizeof(cfg));
		error = pmc_pmu_pmcallocate(event->name, &cfg);
		if (error == 0) {
			printf("UMC PMU event %s produced an allocation descriptor; "
			    "runtime support may be present on this tree\n",
			    event->name);
			return;
		}
		if (error == EOPNOTSUPP)
			saw_eopnotsupp = true;
		else if (error == ENOENT)
			saw_enoent = true;
	}

	ATF_REQUIRE_MSG(saw_eopnotsupp,
	    "Expected at least one Zen 4/5/6 UMC event to reach libpmc and stop "
	    "at EOPNOTSUPP; saw_enoent=%s", saw_enoent ? "true" : "false");
	printf("UMC metadata is present, but runtime allocation is not available.\n");
}

ATF_TC(umcdf_umc_runtime_smoke_if_supported);
ATF_TC_HEAD(umcdf_umc_runtime_smoke_if_supported, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Smoke test AMD UMC PMC runtime when a backend is available");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(umcdf_umc_runtime_smoke_if_supported, tc)
{
	const struct amd_umcdf_event_candidate *event;
	struct pmc_op_pmcallocate cfg;
	struct amd_umcdf_cpu cpu;
	pmc_value_t after, before;
	pmc_id_t pmcid;
	int error, last_error;

	pmcid = PMC_ID_INVALID;
	before = after = 0;

	amd_umcdf_skip_unless_known_zen(&cpu);
	if (!amd_umcdf_has_freebsd_umc_json(&cpu))
		atf_tc_skip("No FreeBSD UMC PMU JSON is expected for %s",
		    amd_umcdf_zen_name(cpu.zen));
	amd_umcdf_skip_unless_pmu_events();

	event = amd_umcdf_pick_pmu_event(umc_events, &cfg, &last_error);
	if (event == NULL)
		atf_tc_skip("UMC PMU metadata is not allocatable by this libpmc/hwpmc "
		    "tree yet; last error was %s", strerror(last_error));

	error = pmc_allocate(event->name, PMC_MODE_SC, 0, 0, &pmcid, 0);
	if (error < 0 && (errno == ENOENT || errno == EOPNOTSUPP ||
	    errno == ENXIO || errno == EBUSY))
		atf_tc_skip("pmc_allocate(%s) is not available in this test "
		    "environment: %s", event->name, strerror(errno));
	ATF_REQUIRE_MSG(error == 0,
	    "pmc_allocate(%s) failed after PMU metadata allocation succeeded: %s",
	    event->name, strerror(errno));

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
	error = amd_umcdf_generate_memory_traffic(128 * 1024 * 1024, 4);
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
	    "UMC counter moved backwards: before=%ju after=%ju",
	    (uintmax_t)before, (uintmax_t)after);
	amd_umcdf_release_pmc(pmcid);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, umcdf_umc_capability_probe);
	ATF_TP_ADD_TC(tp, umcdf_umc_pmu_metadata_contract);
	ATF_TP_ADD_TC(tp, umcdf_umc_runtime_smoke_if_supported);
	return (atf_no_error());
}

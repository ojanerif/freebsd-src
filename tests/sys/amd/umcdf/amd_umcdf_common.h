/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: davi.chavesazevedo@amd.com
 *
 * Common AMD UMC/DF hwpmc test helpers.
 *
 * These tests intentionally detect the AMD family/model/stepping before any
 * PMU-event or hwpmc allocation work.  Family 1Ah is split between Zen 5 and
 * Zen 6, so model decoding is part of the test contract.
 *
 * Pure decode helpers (no hardware I/O) live in amd_umcdf_decode.h and are
 * included here so that all hardware tests get the full API.
 */

#ifndef _AMD_UMCDF_COMMON_H_
#define	_AMD_UMCDF_COMMON_H_

#include "amd_umcdf_decode.h"

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>
#include <sys/pmc.h>

#include <machine/pmc_mdep.h>

#include <atf-c.h>

#include <errno.h>
#include <fcntl.h>
#include <pmc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static volatile uint64_t amd_umcdf_sink;

static inline int
amd_umcdf_do_cpuid(uint32_t level, uint32_t regs[4])
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

static inline int
amd_umcdf_read_cpu(struct amd_umcdf_cpu *cpu)
{
	uint32_t base_family, base_model, ext_family, ext_model;
	uint32_t regs[4];
	int error;

	memset(cpu, 0, sizeof(*cpu));
	cpu->zen = AMD_UMCDF_ZEN_UNKNOWN;

	error = amd_umcdf_do_cpuid(AMD_UMCDF_CPUID_VENDOR, regs);
	if (error != 0)
		return (error);

	cpu->is_amd = regs[1] == AMD_UMCDF_VENDOR_EBX &&
	    regs[3] == AMD_UMCDF_VENDOR_EDX && regs[2] == AMD_UMCDF_VENDOR_ECX;
	if (!cpu->is_amd)
		return (0);

	error = amd_umcdf_do_cpuid(AMD_UMCDF_CPUID_FMS, regs);
	if (error != 0)
		return (error);

	base_family = (regs[0] >> 8) & 0x0f;
	ext_family = (regs[0] >> 20) & 0xff;
	base_model = (regs[0] >> 4) & 0x0f;
	ext_model = (regs[0] >> 16) & 0x0f;
	cpu->stepping = regs[0] & 0x0f;
	cpu->family = (base_family == 0x0f) ? base_family + ext_family :
	    base_family;
	cpu->model = (base_family == 0x0f || base_family == 0x06) ?
	    (ext_model << 4) | base_model : base_model;
	cpu->zen = amd_umcdf_map_zen(cpu->family, cpu->model);

	error = amd_umcdf_do_cpuid(AMD_UMCDF_CPUID_EXTMAX, regs);
	if (error != 0)
		return (error);
	cpu->ext_high = regs[0];

	if (cpu->ext_high >= AMD_UMCDF_CPUID_EXTFEATURES) {
		error = amd_umcdf_do_cpuid(AMD_UMCDF_CPUID_EXTFEATURES, regs);
		if (error != 0)
			return (error);
		cpu->amd_feature2_ecx = regs[2];
	}

	return (0);
}

static inline void
amd_umcdf_skip_unless_amd(struct amd_umcdf_cpu *cpu)
{
	int error;

	error = amd_umcdf_read_cpu(cpu);
	if (error != 0)
		atf_tc_skip("Cannot query CPUID via /dev/cpuctl0: %s",
		    strerror(error));
	if (!cpu->is_amd)
		atf_tc_skip("CPU vendor is not AuthenticAMD");
}

static inline void
amd_umcdf_skip_unless_known_zen(struct amd_umcdf_cpu *cpu)
{
	amd_umcdf_skip_unless_amd(cpu);
	if (cpu->zen == AMD_UMCDF_ZEN_PRE_ZEN)
		atf_tc_skip("AMD Family %02xh is pre-Zen; UMC/DF test is not applicable",
		    cpu->family);
	if (cpu->zen == AMD_UMCDF_ZEN_FUTURE)
		atf_tc_skip("AMD Family %02xh is newer than the validated Zen map; "
		    "probe only after checking the PPR", cpu->family);
	if (cpu->zen == AMD_UMCDF_ZEN_UNKNOWN)
		atf_tc_skip("AMD Family %02xh Model %02xh is not in the validated "
		    "Zen map", cpu->family, cpu->model);
}

static inline void
amd_umcdf_skip_unless_hwpmc(void)
{
	if (pmc_init() < 0)
		atf_tc_skip("pmc_init() failed; is hwpmc loaded? %s",
		    strerror(errno));
}

static inline void
amd_umcdf_skip_unless_pmu_events(void)
{
	amd_umcdf_skip_unless_hwpmc();
	if (!pmc_pmu_enabled())
		atf_tc_skip("pmu-events support is not enabled in this hwpmc setup");
}

static inline int
amd_umcdf_read_perfmon_v2(const struct amd_umcdf_cpu *cpu,
    struct amd_umcdf_perfmon_v2 *pmv2)
{
	uint32_t regs[4];
	int error;

	memset(pmv2, 0, sizeof(*pmv2));
	if (cpu->ext_high < AMD_UMCDF_CPUID_EXTPERFMON)
		return (0);

	error = amd_umcdf_do_cpuid(AMD_UMCDF_CPUID_EXTPERFMON, regs);
	if (error != 0)
		return (error);

	pmv2->leaf_available = true;
	pmv2->raw_eax = regs[0];
	pmv2->raw_ebx = regs[1];
	pmv2->raw_ecx = regs[2];
	pmv2->raw_edx = regs[3];
	pmv2->present = (regs[0] & AMD_UMCDF_EXTPERFMON_PRESENT) != 0;
	pmv2->core_pmcs = AMD_UMCDF_EXTPERFMON_CORE_PMCS(regs[1]);
	pmv2->lbr_v2_depth = AMD_UMCDF_EXTPERFMON_LBR_V2_DEPTH(regs[1]);
	pmv2->df_pmcs = AMD_UMCDF_EXTPERFMON_DF_PMCS(regs[1]);
	pmv2->active_umc_mask = regs[2];
	return (0);
}

static inline size_t
amd_umcdf_count_pmc_rows_with_prefix(int cpu, const char *prefix)
{
	struct pmc_pmcinfo *pmcinfo;
	size_t count, prefix_len;
	int i, npmc;

	ATF_REQUIRE_MSG(pmc_pmcinfo(cpu, &pmcinfo) == 0,
	    "pmc_pmcinfo(%d) failed: %s", cpu, strerror(errno));
	npmc = pmc_npmc(cpu);
	if (npmc < 0) {
		free(pmcinfo);
		atf_tc_fail("pmc_npmc(%d) failed: %s", cpu, strerror(errno));
	}

	count = 0;
	prefix_len = strlen(prefix);
	for (i = 0; i < npmc; i++) {
		if (strncmp(pmcinfo->pm_pmcs[i].pm_name, prefix,
		    prefix_len) == 0)
			count++;
	}

	free(pmcinfo);
	return (count);
}

static inline const struct amd_umcdf_event_candidate *
amd_umcdf_pick_pmu_event(const struct amd_umcdf_event_candidate *candidates,
    struct pmc_op_pmcallocate *cfg, int *last_error)
{
	const struct amd_umcdf_event_candidate *cand;
	int error;

	if (last_error != NULL)
		*last_error = ENOENT;
	for (cand = candidates; cand->name != NULL; cand++) {
		memset(cfg, 0, sizeof(*cfg));
		error = pmc_pmu_pmcallocate(cand->name, cfg);
		if (error == 0)
			return (cand);
		if (last_error != NULL)
			*last_error = error;
	}

	return (NULL);
}

static inline int
amd_umcdf_generate_memory_traffic(size_t bytes, unsigned int rounds)
{
	uint8_t *buf;
	size_t i;
	unsigned int r;

	if (bytes < AMD_UMCDF_CACHELINE)
		return (EINVAL);
	buf = (uint8_t *)malloc(bytes);
	if (buf == NULL)
		return (ENOMEM);

	memset(buf, 0xa5, bytes);
	for (r = 0; r < rounds; r++) {
		for (i = 0; i < bytes; i += AMD_UMCDF_CACHELINE) {
			buf[i] = (uint8_t)(buf[i] + r + (i >> 6));
			amd_umcdf_sink += buf[i];
		}
	}

	free(buf);
	return (0);
}

static inline void
amd_umcdf_release_pmc(pmc_id_t pmcid)
{
	if (pmcid != PMC_ID_INVALID)
		ATF_REQUIRE_MSG(pmc_release(pmcid) == 0,
		    "pmc_release(%u) failed: %s", pmcid, strerror(errno));
}

#endif /* _AMD_UMCDF_COMMON_H_ */

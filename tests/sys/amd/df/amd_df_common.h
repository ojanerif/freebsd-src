/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: ojanerif@amd.com
 *
 * Common AMD Data Fabric PMU test helpers.
 *
 * Reuses the UMCDF infrastructure (Zen-generation detection, CPUID access,
 * PMC lifecycle helpers) via an -I flag pointing at tests/sys/amd/umcdf/.
 * DF-specific workload generators and event candidates are defined here.
 *
 * Pure decode helpers (no I/O) remain in amd_umcdf_decode.h and are pulled
 * in transitively through amd_umcdf_common.h.
 *
 * Jira: FreeBSD-Tests-023
 */

#ifndef _AMD_DF_COMMON_H_
#define	_AMD_DF_COMMON_H_

#include "amd_umcdf_common.h"

/*
 * Workload sizing for DF interconnect traffic generation.
 *
 * REMOTE_SIZE: 128 MB with a page stride forces repeated DRAM accesses that
 * cross NUMA domains on multi-die EPYC systems (e.g. Genoa 4-die), generating
 * measurable DF interconnect traffic.  On single-die parts the workload still
 * exercises the local UMC path and produces non-zero DF counter increments.
 *
 * LOCAL_SIZE: 32 MB fits within one NUMA node's DRAM bandwidth and is used
 * for the local-traffic baseline in interconnect comparison tests.
 */
#define	AMD_DF_REMOTE_BUFFER_SIZE	(128 * 1024 * 1024UL)
#define	AMD_DF_REMOTE_PAGE_STRIDE	4096U
#define	AMD_DF_REMOTE_ROUNDS		4U

#define	AMD_DF_LOCAL_BUFFER_SIZE	(32 * 1024 * 1024UL)
#define	AMD_DF_LOCAL_CACHELINE_STRIDE	AMD_UMCDF_CACHELINE
#define	AMD_DF_LOCAL_ROUNDS		8U

/*
 * DF PMU event candidates — ordered most-specific (newest Zen) first so
 * amd_umcdf_pick_pmu_event() finds the best match for the running CPU.
 *
 * Events cover DRAM-channel traffic across Zen 1–5; the _0 suffix selects
 * controller/channel 0.  All are domain-wide (K8-DF class in hwpmc).
 */
static const struct amd_umcdf_event_candidate amd_df_dram_events[] = {
	{
		"local_or_remote_socket_read_data_beats_dram_0",
		"Zen 5 DF PMU: data beats to DRAM channel 0",
		0x1f, 0xffe
	},
	{
		"local_processor_read_data_beats_cs0",
		"Zen 4 DF PMU: local read data beats, coherent slave 0",
		0x1f, 0x7fe
	},
	{
		"dram_channel_data_controller_0",
		"Zen 1/2/3 DF PMU: DRAM channel data, controller 0",
		0x07, 0x38
	},
	{
		"remote_outbound_data_controller_0",
		"Zen 1/2/3 DF PMU: remote outbound data, controller 0",
		0x7c7, 0x03
	},
	{ NULL, NULL, 0, 0 }
};

/*
 * Event candidates for interconnect (fabric link) traffic.
 * Used by df_interconnect_test to probe cross-die data movement.
 */
static const struct amd_umcdf_event_candidate amd_df_link_events[] = {
	{
		"local_or_remote_socket_read_data_beats_dram_1",
		"Zen 5 DF PMU: data beats DRAM channel 1 (second controller)",
		0x1f, 0xffe
	},
	{
		"local_processor_read_data_beats_cs1",
		"Zen 4 DF PMU: local read data beats, coherent slave 1",
		0x1f, 0x7fe
	},
	{
		"dram_channel_data_controller_1",
		"Zen 1/2/3 DF PMU: DRAM channel data, controller 1",
		0x07, 0x38
	},
	{ NULL, NULL, 0, 0 }
};

/*
 * Returns true if FreeBSD ships DF PMU event JSON for this Zen generation.
 * Delegates to the UMCDF helper which covers Zen 1–5.
 */
static inline bool
amd_df_has_freebsd_df_json(const struct amd_umcdf_cpu *cpu)
{
	return (amd_umcdf_has_freebsd_df_json(cpu));
}

/*
 * Generate cross-NUMA memory traffic to exercise the DF interconnect.
 *
 * Allocates AMD_DF_REMOTE_BUFFER_SIZE bytes and accesses it at page stride.
 * On multi-die EPYC systems pages beyond the first NUMA node's local memory
 * cause remote reads that traverse the Infinity Fabric, incrementing DF
 * interconnect counters.  On single-die parts the workload exercises the
 * local DRAM path.
 */
static inline int
amd_df_generate_remote_traffic(void)
{
	uint8_t *buf;
	size_t i;
	unsigned int r;

	buf = (uint8_t *)malloc(AMD_DF_REMOTE_BUFFER_SIZE);
	if (buf == NULL)
		return (ENOMEM);

	memset(buf, 0x5a, AMD_DF_REMOTE_BUFFER_SIZE);

	for (r = 0; r < AMD_DF_REMOTE_ROUNDS; r++) {
		for (i = 0; i < AMD_DF_REMOTE_BUFFER_SIZE;
		    i += AMD_DF_REMOTE_PAGE_STRIDE) {
			buf[i] = (uint8_t)(buf[i] ^ (uint8_t)(r + (i >> 12)));
			amd_umcdf_sink += buf[i];
		}
	}

	free(buf);
	return (0);
}

/*
 * Generate local DRAM traffic (cacheline-stride, fits in one NUMA node).
 * Used as a baseline for interconnect comparison tests.
 */
static inline int
amd_df_generate_local_traffic(void)
{
	uint8_t *buf;
	size_t i;
	unsigned int r;

	buf = (uint8_t *)malloc(AMD_DF_LOCAL_BUFFER_SIZE);
	if (buf == NULL)
		return (ENOMEM);

	memset(buf, 0xa5, AMD_DF_LOCAL_BUFFER_SIZE);

	for (r = 0; r < AMD_DF_LOCAL_ROUNDS; r++) {
		for (i = 0; i < AMD_DF_LOCAL_BUFFER_SIZE;
		    i += AMD_DF_LOCAL_CACHELINE_STRIDE) {
			buf[i] = (uint8_t)(buf[i] + (uint8_t)(r + i));
			amd_umcdf_sink += buf[i];
		}
	}

	free(buf);
	return (0);
}

#endif /* _AMD_DF_COMMON_H_ */

/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * Minimal IBS Op + Fetch MSR interface for the standalone stress suite.
 *
 * Uses /dev/cpuctl{N} via CPUCTL_RDMSR / CPUCTL_WRMSR ioctl — root required.
 * No libpmc dependency; this header is self-contained.
 *
 * IBS Op symbols:
 *   SIBS_MSR_OP_CTL / SIBS_MSR_OP_RIP / SIBS_MSR_OP_DATA
 *   SIBS_OP_EN          — IbsOpEn bit (enables sampling)
 *   SIBS_OP_VAL         — IbsOpVal bit (sample ready, set by hardware)
 *   SIBS_MAXCNT_MASK    — bits[15:0] MaxCnt field
 *   SIBS_OP_RIP_INVALID — bit 38 of MSR_IBS_OP_DATA; RIP is invalid when set
 *   SIBS_SAFE_PERIOD    — conservative Op sampling period
 *                         (MaxCnt=0x1000 → 65536 retired µops between samples)
 *   sibs_cpu_supports_ibs_op()  — CPUID 0x8000001B EAX[2] check
 *   sibs_op_disable()           — workaround #420 drain on MSR_IBS_OP_CTL
 *
 * IBS Fetch symbols:
 *   SIBS_MSR_FETCH_CTL / SIBS_MSR_FETCH_LINADDR
 *   SIBS_FETCH_EN            — IbsFetchEn bit (enables sampling)
 *   SIBS_FETCH_VAL           — IbsFetchVal bit (sample ready, set by hardware)
 *   SIBS_FETCH_MAXCNT_MASK   — bits[15:0] MaxCnt field
 *   SIBS_FETCH_SAFE_PERIOD   — conservative Fetch sampling period
 *                              (MaxCnt=0x1000 → 65536 fetch clocks between samples)
 *   sibs_cpu_supports_ibs_fetch() — CPUID 0x8000001B EAX[0] check
 *   sibs_fetch_disable()          — drain sequence on MSR_IBS_FETCH_CTL
 *
 * Common helpers:
 *   sibs_read_msr() / sibs_write_msr() — cpuctl ioctl wrappers
 */

#ifndef _STRESS_IBS_H_
#define _STRESS_IBS_H_

#include <sys/types.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* ── IBS Fetch MSR addresses ────────────────────────────────────────────── */
#define SIBS_MSR_FETCH_CTL	0xC0011030	/* IBS Fetch Control */
#define SIBS_MSR_FETCH_LINADDR	0xC0011031	/* IBS Fetch Linear Address */

/* ── IBS Fetch CTL bit fields (MSR 0xC0011030) ─────────────────────────── */
#define SIBS_FETCH_MAXCNT_MASK	0x000000000000FFFFULL	/* bits[15:0]: MaxCnt */
#define SIBS_FETCH_EN		(1ULL << 48)		/* IbsFetchEn */
#define SIBS_FETCH_VAL		(1ULL << 49)		/* IbsFetchVal — sample ready */

/* ── CPUID for IBS Fetch detection ─────────────────────────────────────── */
#define SIBS_CPUID_FETCHSAM_BIT	(1U << 0)		/* EAX[0]: IBS Fetch sampling */

/*
 * Conservative sampling period for IBS Fetch stress tests.
 * MaxCnt = 0x1000 → 0x1000 × 16 = 65 536 fetch clocks between samples.
 * Keeps NMI rate consistent with the Op safe period (SIBS_SAFE_PERIOD).
 */
#define SIBS_FETCH_SAFE_PERIOD	0x1000ULL

/* ── IBS Op MSR addresses ──────────────────────────────────────────────── */
#define SIBS_MSR_OP_CTL		0xC0011033	/* IBS Op Control */
#define SIBS_MSR_OP_RIP		0xC0011034	/* IBS Op Linear IP */
#define SIBS_MSR_OP_DATA	0xC0011035	/* IBS Op Data 1 */

/* ── IBS Op CTL bit fields (MSR 0xC0011033) ────────────────────────────── */
#define SIBS_MAXCNT_MASK	0x000000000000FFFFULL	/* bits[15:0]: MaxCnt */
#define SIBS_OP_EN		(1ULL << 17)		/* IbsOpEn */
#define SIBS_OP_VAL		(1ULL << 18)		/* IbsOpVal — sample ready */

/* ── IBS Op Data bit fields (MSR 0xC0011035) ───────────────────────────── */
#define SIBS_OP_RIP_INVALID	(1ULL << 38)		/* IbsRipInvalid */

/* ── CPUID for IBS Op detection ────────────────────────────────────────── */
#define SIBS_CPUID_IBS_ID	0x8000001BU
#define SIBS_CPUID_OPSAM_BIT	(1U << 2)		/* EAX[2]: IBS Op sampling */

/*
 * Conservative sampling period for stress tests.
 * MaxCnt = 0x1000 → 0x1000 × 16 = 65 536 retired micro-ops between samples.
 * Keeps NMI rate well below the storm threshold documented in FreeBSD-Tests-026.
 */
#define SIBS_SAFE_PERIOD	0x1000ULL

/* ── cpuctl helpers ────────────────────────────────────────────────────── */

static inline int
sibs_read_msr(int cpu, uint32_t reg, uint64_t *val)
{
	cpuctl_msr_args_t a;
	char path[32];
	int fd, err;

	snprintf(path, sizeof(path), "/dev/cpuctl%d", cpu);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return (errno);
	a.msr = reg;
	if (ioctl(fd, CPUCTL_RDMSR, &a) < 0) {
		err = errno;
		(void)close(fd);
		return (err);
	}
	*val = a.data;
	(void)close(fd);
	return (0);
}

static inline int
sibs_write_msr(int cpu, uint32_t reg, uint64_t val)
{
	cpuctl_msr_args_t a;
	char path[32];
	int fd, err;

	snprintf(path, sizeof(path), "/dev/cpuctl%d", cpu);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return (errno);
	a.msr = reg;
	a.data = val;
	if (ioctl(fd, CPUCTL_WRMSR, &a) < 0) {
		err = errno;
		(void)close(fd);
		return (err);
	}
	(void)close(fd);
	return (0);
}

/*
 * Returns true if the CPU supports IBS Op sampling.
 * Checks CPUID 0x8000001B EAX[2] (IbsOpSam) via /dev/cpuctl0.
 */
static inline bool
sibs_cpu_supports_ibs_op(void)
{
	cpuctl_cpuid_args_t a;
	int fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (false);
	a.level = SIBS_CPUID_IBS_ID;
	if (ioctl(fd, CPUCTL_CPUID, &a) < 0) {
		(void)close(fd);
		return (false);
	}
	(void)close(fd);
	return ((a.data[0] & SIBS_CPUID_OPSAM_BIT) != 0);
}

/*
 * Returns true if the CPU supports IBS Fetch sampling.
 * Checks CPUID 0x8000001B EAX[0] (IbsFetchSam) via /dev/cpuctl0.
 */
static inline bool
sibs_cpu_supports_ibs_fetch(void)
{
	cpuctl_cpuid_args_t a;
	int fd;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (false);
	a.level = SIBS_CPUID_IBS_ID;
	if (ioctl(fd, CPUCTL_CPUID, &a) < 0) {
		(void)close(fd);
		return (false);
	}
	(void)close(fd);
	return ((a.data[0] & SIBS_CPUID_FETCHSAM_BIT) != 0);
}

/*
 * Disable IBS Fetch on `cpu`:
 *   1. Clear MaxCnt — stop arming new samples.
 *   2. Clear IbsFetchEn — disable the sampler.
 *   3. 10 × 1 µs zero writes — drain any in-flight fetch NMI.
 *
 * IBS Fetch has no AMD erratum #420 equivalent; a short drain is sufficient.
 */
static inline void
sibs_fetch_disable(int cpu)
{
	uint64_t ctl;
	int i;

	if (sibs_read_msr(cpu, SIBS_MSR_FETCH_CTL, &ctl) != 0)
		return;
	ctl &= ~SIBS_FETCH_MAXCNT_MASK;
	(void)sibs_write_msr(cpu, SIBS_MSR_FETCH_CTL, ctl);
	usleep(1);
	ctl &= ~SIBS_FETCH_EN;
	(void)sibs_write_msr(cpu, SIBS_MSR_FETCH_CTL, ctl);
	for (i = 0; i < 10; i++) {
		(void)sibs_write_msr(cpu, SIBS_MSR_FETCH_CTL, 0);
		usleep(1);
	}
}

/*
 * Disable IBS Op on `cpu` using the AMD workaround #420 stop sequence:
 *   1. Clear MaxCnt — stop arming new samples.
 *   2. Clear IbsOpEn — disable the sampler.
 *   3. 50 × 1 µs zero writes — drain any in-flight NMI.
 *
 * This mirrors the sequence used in FreeBSD's hwpmc_ibs.c ibs_stop_pmc().
 */
static inline void
sibs_op_disable(int cpu)
{
	uint64_t ctl;
	int i;

	if (sibs_read_msr(cpu, SIBS_MSR_OP_CTL, &ctl) != 0)
		return;
	ctl &= ~SIBS_MAXCNT_MASK;
	(void)sibs_write_msr(cpu, SIBS_MSR_OP_CTL, ctl);
	usleep(1);
	ctl &= ~SIBS_OP_EN;
	(void)sibs_write_msr(cpu, SIBS_MSR_OP_CTL, ctl);
	for (i = 0; i < 50; i++) {
		(void)sibs_write_msr(cpu, SIBS_MSR_OP_CTL, 0);
		usleep(1);
	}
}

#endif /* _STRESS_IBS_H_ */

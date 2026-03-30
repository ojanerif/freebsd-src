/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _IBS_UTILS_H_
#define _IBS_UTILS_H_

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>
#include <sys/pciio.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* MSR addresses for IBS */
#define MSR_IBS_FETCH_CTL		0xC0011030
#define MSR_IBS_FETCH_LIN_ADDR		0xC0011031
#define MSR_IBS_FETCH_PHY_ADDR		0xC0011032
#define MSR_IBS_OP_CTL			0xC0011033
#define MSR_IBS_OP_RIP			0xC0011034
#define MSR_IBS_OP_DATA			0xC0011035
#define MSR_IBS_OP_DATA2		0xC0011036
#define MSR_IBS_OP_DATA3		0xC0011037
#define MSR_IBS_DC_LIN_AD		0xC0011038
#define MSR_IBS_DC_PHYS_AD		0xC0011039
#define MSR_IBS_BRANCH_TARGET		0xC001103A
#define MSR_EXT_X86_FEATURE		0xC0011005

/* IBS feature flags */
#define IBS_CPUID_FETCH_SAMPLING	(1 << 0)
#define IBS_CPUID_OP_SAMPLING		(1 << 1)
#define IBS_CPUID_RDWROPCNT		(1 << 2)
#define IBS_CPUID_OPCNT			(1 << 3)
#define IBS_CPUID_BRANCH_TARGET_ADDR	(1 << 4)
#define IBS_CPUID_OP_DATA_4		(1 << 5)
#define IBS_CPUID_ZEN4_IBS		(1 << 6)

/* Helper functions */
static inline int
do_cpuid_ioctl(uint32_t level, uint32_t *regs)
{
	cpuctl_cpuid_args_t args;
	int fd;
	int error;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (errno);

	args.level = level;
	if (ioctl(fd, CPUCTL_CPUID, &args) < 0) {
		error = errno;
		close(fd);
		return (error);
	}

	regs[0] = args.data[0];
	regs[1] = args.data[1];
	regs[2] = args.data[2];
	regs[3] = args.data[3];

	close(fd);
	return (0);
}

static inline bool
cpu_supports_ibs(void)
{
	uint32_t regs[4];

	if (do_cpuid_ioctl(0x80000001, regs) != 0)
		return (false);
	return ((regs[2] & AMDID2_IBS) != 0);
}

static inline bool
cpu_ibs_extended(void)
{
	uint32_t regs[4];

	if (do_cpuid_ioctl(0x8000001B, regs) != 0)
		return (false);
	return ((regs[0] & 0x3f) != 0);
}

static inline bool
cpu_is_zen4(void)
{
	uint32_t regs[4];
	uint32_t family;

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (false);
	family = ((regs[0] >> 8) & 0xf) + ((regs[0] >> 20) & 0xff);
	return (family == 0x19);
}

static inline int
read_msr(int cpu, uint32_t reg, uint64_t *val)
{
	cpuctl_msr_args_t args;
	char dev_path[32];
	int fd;
	int error;

	snprintf(dev_path, sizeof(dev_path), "/dev/cpuctl%d", cpu);
	fd = open(dev_path, O_RDWR);
	if (fd < 0)
		return (errno);

	args.msr = reg;
	if (ioctl(fd, CPUCTL_RDMSR, &args) < 0) {
		error = errno;
		close(fd);
		return (error);
	}

	*val = args.data;
	close(fd);
	return (0);
}

static inline int
write_msr(int cpu, uint32_t reg, uint64_t val)
{
	cpuctl_msr_args_t args;
	char dev_path[32];
	int fd;
	int error;

	snprintf(dev_path, sizeof(dev_path), "/dev/cpuctl%d", cpu);
	fd = open(dev_path, O_RDWR);
	if (fd < 0)
		return (errno);

	args.msr = reg;
	args.data = val;
	if (ioctl(fd, CPUCTL_WRMSR, &args) < 0) {
		error = errno;
		close(fd);
		return (error);
	}

	close(fd);
	return (0);
}

#endif /* _IBS_UTILS_H_ */

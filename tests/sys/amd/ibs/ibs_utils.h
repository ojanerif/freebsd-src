/*-
 * Copyright (c) 2023, 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
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

/* AMD IBS MSR's - duplicated here for test compilation */
#define	MSR_AMD64_IBSFETCHCTL		0xc0011030
#define	MSR_AMD64_IBSFETCHLINAD		0xc0011031
#define	MSR_AMD64_IBSFETCHPHYSAD	0xc0011032
#define	MSR_AMD64_IBSFETCH_REG_COUNT	3
#define	MSR_AMD64_IBSFETCH_REG_MASK	((1UL<<MSR_AMD64_IBSFETCH_REG_COUNT)-1)
#define	MSR_AMD64_IBSOPCTL		0xc0011033
#define	MSR_AMD64_IBSOPRIP		0xc0011034
#define	MSR_AMD64_IBSOPDATA		0xc0011035
#define	MSR_AMD64_IBSOPDATA2		0xc0011036
#define	MSR_AMD64_IBSOPDATA3		0xc0011037
#define	MSR_AMD64_IBSDCLINAD		0xc0011038
#define	MSR_AMD64_IBSDCPHYSAD		0xc0011039
#define	MSR_AMD64_IBSOP_REG_COUNT	7
#define	MSR_AMD64_IBSOP_REG_MASK	((1UL<<MSR_AMD64_IBSOP_REG_COUNT)-1)
#define	MSR_AMD64_IBSCTL		0xc001103a
#define	MSR_AMD64_IBSBRTARGET		0xc001103b
#define	MSR_AMD64_ICIBSEXTDCTL		0xc001103c
#define	MSR_AMD64_IBSOPDATA4		0xc001103d
#define	MSR_AMD64_IBS_REG_COUNT_MAX	8 /* includes MSR_AMD64_IBSBRTARGET */

/* IBS feature flags */
#define IBS_CPUID_FETCH_SAMPLING	(1 << 0)
#define IBS_CPUID_OP_SAMPLING		(1 << 1)
#define IBS_CPUID_RDWROPCNT		(1 << 2)
#define IBS_CPUID_OPCNT			(1 << 3)
#define IBS_CPUID_BRANCH_TARGET_ADDR	(1 << 4)
#define IBS_CPUID_OP_DATA_4		(1 << 5)
#define IBS_CPUID_ZEN4_IBS		(1 << 6)

/* CPUID constants for IBS */
#define CPUID_IBSID			0x8000001B
#define CPUID_IBSID_IBSFETCHCTLEXTD	0x00000020 /* IBS Fetch Control Ext */
#define CPUID_IBSID_IBSOPDATA4		0x00000040 /* IBS OP DATA4 */
#define CPUID_IBSID_ZEN4IBSEXTENSIONS	0x00000080 /* IBS Zen 4 Extensions */
#define CPUID_IBSID_IBSLOADLATENCYFILT	0x00000100 /* Load Latency Filtering */

/* AMD CPUID feature flags */
#define AMDID2_IBS			0x00000400

/* IBS Control register bit fields */
#define IBS_FETCH_MAXCNT		0x000000000000ffffULL	/* Bits 0-15: Max count */
#define IBS_FETCH_CNT			0x00000000ffff0000ULL	/* Bits 16-31: Current count */
#define IBS_FETCH_LAT			0x0000ffff00000000ULL	/* Bits 32-47: Fetch latency */
#define IBS_FETCH_EN			0x0001000000000000ULL	/* Bit 48: Fetch enable */
#define IBS_FETCH_VAL			0x0002000000000000ULL	/* Bit 49: Fetch valid */
#define IBS_FETCH_COMP			0x0004000000000000ULL	/* Bit 50: Fetch complete */
#define IBS_IC_MISS			0x0008000000000000ULL	/* Bit 51: I-cache miss */
#define IBS_PHY_ADDR_VALID		0x0010000000000000ULL	/* Bit 52: Physical addr valid */
#define IBS_L1TLB_PGSZ			0x0060000000000000ULL	/* Bits 53-54: L1TLB page size */
#define IBS_L1TLB_PGSZ_SHIFT		53
#define IBS_L1TLB_MISS			0x0080000000000000ULL	/* Bit 55: L1TLB miss */
#define IBS_L2TLB_MISS			0x0100000000000000ULL	/* Bit 56: L2TLB miss */
#define IBS_RAND_EN			0x0200000000000000ULL	/* Bit 57: Random enable */
#define IBS_FETCH_L2_MISS		0x0400000000000000ULL	/* Bit 58: L2 miss */
#define IBS_L3_MISS_ONLY		0x0800000000000000ULL	/* Bit 59: L3 miss only */

/* IBS Op Control bit fields */
#define IBS_OP_MAXCNT			0x000000000000ffffULL	/* Bits 0-15: Op max count */
#define IBS_OP_L3_MISS_ONLY		0x0000000000010000ULL	/* Bit 16: L3 miss only */
#define IBS_OP_EN			0x0000000000020000ULL	/* Bit 17: Op enable */
#define IBS_OP_VAL			0x0000000000040000ULL	/* Bit 18: Op valid */
#define IBS_CNT_CTL			0x0000000000080000ULL	/* Bit 19: Counter control */
#define IBS_OP_MAXCNT_EXT		0x0000000007f00000ULL	/* Bits 20-26: Max count ext */
#define IBS_OP_MAXCNT_EXT_SHIFT		20
#define IBS_OP_CURCNT			0x07ffffff00000000ULL	/* Bits 32-58: Current count */
#define IBS_LDLAT_THRSH			0x7800000000000000ULL	/* Bits 59-62: Load latency thresh */
#define IBS_LDLAT_THRSH_SHIFT		59
#define IBS_LDLAT_EN			0x8000000000000000ULL	/* Bit 63: Load latency enable */

/* IBS Op Data 1 bit fields */
#define IBS_COMP_TO_RET_CTR		0x000000000000ffffULL	/* Bits 0-15: Completion to retire */
#define IBS_TAG_TO_RET_CTR		0x00000000ffff0000ULL	/* Bits 16-31: Tag to retire */
#define IBS_OP_RETURN			0x0000000400000000ULL	/* Bit 34: Return op */
#define IBS_OP_BRN_TAKEN		0x0000000800000000ULL	/* Bit 35: Taken branch */
#define IBS_OP_BRN_MISP			0x0000001000000000ULL	/* Bit 36: Mispredicted branch */
#define IBS_OP_BRN_RET			0x0000002000000000ULL	/* Bit 37: Branch retired */
#define IBS_OP_RIP_INVALID		0x0000004000000000ULL	/* Bit 38: RIP invalid */
#define IBS_OP_BRN_FUSE			0x0000008000000000ULL	/* Bit 39: Fused branch */
#define IBS_OP_MICROCODE		0x0000010000000000ULL	/* Bit 40: Microcode op */

/* IBS Op Data 2 bit fields */
#define IBS_DATA_SRC_LO			0x0000000000000007ULL	/* Bits 0-2: Data source low */
#define IBS_RMT_NODE			0x0000000000000010ULL	/* Bit 4: Remote node */
#define IBS_CACHE_HIT_ST		0x0000000000000020ULL	/* Bit 5: Cache hit state */
#define IBS_DATA_SRC_HI			0x00000000000000c0ULL	/* Bits 6-7: Data source high */

/* IBS Op Data 3 bit fields */
#define IBS_LD_OP			0x0000000000000001ULL	/* Bit 0: Load op */
#define IBS_ST_OP			0x0000000000000002ULL	/* Bit 1: Store op */
#define IBS_DC_L1TLB_MISS		0x0000000000000004ULL	/* Bit 2: DC L1TLB miss */
#define IBS_DC_L2TLB_MISS		0x0000000000000008ULL	/* Bit 3: DC L2TLB miss */
#define IBS_DC_L1TLB_HIT_2M		0x0000000000000010ULL	/* Bit 4: DC L1TLB hit 2M */
#define IBS_DC_L1TLB_HIT_1G		0x0000000000000020ULL	/* Bit 5: DC L1TLB hit 1G */
#define IBS_DC_L2TLB_HIT_2M		0x0000000000000040ULL	/* Bit 6: DC L2TLB hit 2M */
#define IBS_DC_MISS			0x0000000000000080ULL	/* Bit 7: DC miss */
#define IBS_DC_MIS_ACC			0x0000000000000100ULL	/* Bit 8: Misaligned access */
#define IBS_DC_WC_MEM_ACC		0x0000000000002000ULL	/* Bit 13: Write combining */
#define IBS_DC_UC_MEM_ACC		0x0000000000004000ULL	/* Bit 14: Uncacheable */
#define IBS_DC_LOCKED_OP		0x0000000000008000ULL	/* Bit 15: Locked op */
#define IBS_DC_MISS_NO_MAB_ALLOC	0x0000000000010000ULL	/* Bit 16: No MAB alloc */
#define IBS_DC_LIN_ADDR_VALID		0x0000000000020000ULL	/* Bit 17: Linear addr valid */
#define IBS_DC_PHY_ADDR_VALID		0x0000000000040000ULL	/* Bit 18: Physical addr valid */
#define IBS_DC_L2_TLB_HIT_1G		0x0000000000080000ULL	/* Bit 19: L2 TLB hit 1G */
#define IBS_L2_MISS			0x0000000000100000ULL	/* Bit 20: L2 miss */
#define IBS_SW_PF			0x0000000000200000ULL	/* Bit 21: Software prefetch */
#define IBS_OP_MEM_WIDTH		0x0000000003c00000ULL	/* Bits 22-25: Mem width */
#define IBS_OP_MEM_WIDTH_SHIFT		22
#define IBS_OP_DC_MISS_OPEN_MEM_REQS	0x00000000fc000000ULL	/* Bits 26-31: Outstanding reqs */
#define IBS_DC_MISS_LAT			0x0000ffff00000000ULL	/* Bits 32-47: DC miss latency */
#define IBS_TLB_REFILL_LAT		0xffff000000000000ULL	/* Bits 48-63: TLB refill latency */

/* IBS Op Data 4 (Zen 4+) bit fields */
#define IBS_OP_DATA4_VALID		0x0000000000000001ULL	/* Bit 0: Data valid */
#define IBS_OP_DATA4_REMOTE_LAT		0x000000000000fffeULL	/* Bits 1-16: Remote latency */
#define IBS_OP_DATA4_REMOTE_LAT_SHIFT	1

/* IBS Global Control (MSR 0xc001103a) bit definitions */
#define IBSCTL_FETCH_EN			(1ULL << 0)	/* Global fetch enable */
#define IBSCTL_OP_EN			(1ULL << 1)	/* Global op enable */
#define IBSCTL_VALID_BITS		0x3ULL		/* Valid control bits */

/* Additional IBS constants for tests */
#define IBS_MAXCNT_MASK			0x000000000000FFFFULL
#define IBS_FETCH_ENABLE_BIT		(1ULL << 48)	/* IBS_FETCH_EN */
#define IBS_OP_ENABLE_BIT		(1ULL << 17)	/* IBS_OP_EN */
#define IBS_FETCH_CTL_ENABLE		(1ULL << 48)	/* IBS_FETCH_EN */
#define IBS_OP_CTL_ENABLE		(1ULL << 17)	/* IBS_OP_EN */
#define IBS_FETCH_CNT			0x00000000ffff0000ULL	/* Current count field */
#define IBS_OP_MAXCNT			0x000000000000ffffULL	/* Op max count field */

/* PMC ioctl syscall definitions for tests */
#define SYS_hwpmc			548		/* hwpmc syscall number */

/* PMC class definitions */
#define PMC_CLASS_IBS			10		/* IBS PMC class */

/* PMC event definitions */
#define PMC_EV_IBS_FETCH		(PMC_EV_IBS_FIRST + 0)
#define PMC_EV_IBS_OP			(PMC_EV_IBS_FIRST + 1)
#define PMC_EV_IBS_FIRST		700		/* IBS event base */

/* IBS-specific PMC constants */
#define IBS_PMC_FETCH			0
#define IBS_PMC_OP			1
#define IBS_FETCH_MIN_RATE		1000
#define IBS_OP_MIN_RATE			1000

/* PMC capability flags */
#define PMC_CAP_SYSTEM			0x01
#define PMC_CAP_USER			0x02
#define PMC_CAP_EDGE			0x04
#define PMC_CAP_QUALIFIER		0x08
#define PMC_CAP_PRECISE			0x10
#define PMC_CAP_INTERRUPT		0x20

/* PMC mode definitions */
#define PMC_MODE_SS			1		/* Sampling mode */

/* PMC operation definitions */
#define PMC_OP_PMCALLOCATE		1
#define PMC_OP_PMCSTART			2
#define PMC_OP_PMCSTOP			3
#define PMC_OP_PMCRELEASE		4
#define PMC_OP_IBSGETCAPS		(350)		/* IBS operations */
#define PMC_OP_IBSSETPERIOD		(351)

/* CPUID function definitions */
#define CPUID_IBSID_FETCHSAM		0x00000002
#define CPUID_IBSID_OPSAM		0x00000004
#define CPUID_IBSID_ZEN4IBSEXTENSIONS	0x00000080

/* PMC CPU definitions */
#define PMC_CPU_ANY			-1

/* PMC ID definitions */
#define PMC_ID_INVALID			(~(uint32_t)0)

/* IBS ioctl structures for tests */
struct pmc_op_ibsgetcaps {
	uint32_t pm_ibs_features;
	uint32_t pm_ibs_fetch_cap;
	uint32_t pm_ibs_op_cap;
	uint32_t pm_ibs_zen4_ext;
	uint32_t pm_ibs_load_lat_filt;
	uint32_t pm_ibs_reserved[4];
};

struct pmc_op_ibssetperiod {
	uint32_t pm_pmcid;
	uint64_t pm_period;
};

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
	uint32_t family, model;

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (false);
	family = ((regs[0] >> 8) & 0xf) + ((regs[0] >> 20) & 0xff);
	model = ((regs[0] >> 4) & 0xf) | (((regs[0] >> 16) & 0xf) << 4);
	return (family == 0x19 &&
	    ((model >= 0x10 && model <= 0x1f) ||
	    (model >= 0x60 && model <= 0x7f) ||
	    (model >= 0xa0 && model <= 0xaf)));
}

static inline uint64_t
ibs_get_maxcnt(uint64_t ctl_val)
{
	return (ctl_val & IBS_MAXCNT_MASK);
}

static inline uint64_t
ibs_set_maxcnt(uint64_t ctl_val, uint64_t maxcnt)
{
	return ((ctl_val & ~IBS_MAXCNT_MASK) | (maxcnt & IBS_MAXCNT_MASK));
}

static inline bool
cpu_is_zen5(void)
{
	uint32_t regs[4];
	uint32_t family, model;

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (false);
	family = ((regs[0] >> 8) & 0xf) + ((regs[0] >> 20) & 0xff);
	model = ((regs[0] >> 4) & 0xf) | (((regs[0] >> 16) & 0xf) << 4);
	return (family == 0x1a &&
	    (model <= 0x2f || (model >= 0x40 && model <= 0x4f) ||
	    (model >= 0x60 && model <= 0x7f)));
}

static inline uint32_t
cpu_get_family(void)
{
	uint32_t regs[4];
	uint32_t family;

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (0);
	family = ((regs[0] >> 8) & 0xf) + ((regs[0] >> 20) & 0xff);
	return (family);
}

static inline uint32_t
cpu_get_model(void)
{
	uint32_t regs[4];
	uint32_t model;

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (0);
	model = ((regs[0] >> 4) & 0xf) + ((regs[0] >> 12) & 0xf0);
	return (model);
}

static inline uint32_t
cpu_get_stepping(void)
{
	uint32_t regs[4];

	if (do_cpuid_ioctl(0x1, regs) != 0)
		return (0);
	return (regs[0] & 0xf);
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

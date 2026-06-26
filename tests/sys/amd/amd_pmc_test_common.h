/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Sponsored by: Advanced Micro Devices, Inc.
 * Author: davi.chavesazevedo@amd.com
 *
 * Common helpers for AMD hwpmc tests.
 */

#ifndef _AMD_PMC_TEST_COMMON_H_
#define	_AMD_PMC_TEST_COMMON_H_

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <atf-c.h>

#include <errno.h>
#include <fcntl.h>
#include <pmc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define	AMD_CPUID_VENDOR_LEAF	0x00000000
#define	AMD_CPUID_EXT_BASE	0x80000000
#define	AMD_CPUID_EXT_FEATURES	0x80000001

#define	AMD_CPUID_VENDOR_EBX	0x68747541U /* Auth */
#define	AMD_CPUID_VENDOR_EDX	0x69746e65U /* enti */
#define	AMD_CPUID_VENDOR_ECX	0x444d4163U /* cAMD */

#define	AMDID2_PNXC		0x01000000U
#define	AMDID2_PTSCEL2I		0x10000000U

static inline int
amd_test_do_cpuid(uint32_t level, uint32_t regs[4])
{
	cpuctl_cpuid_args_t args;
	int fd, error;

	fd = open("/dev/cpuctl0", O_RDONLY);
	if (fd < 0)
		return (errno);

	memset(&args, 0, sizeof(args));
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

static inline int
amd_test_get_ext_features_ecx(uint32_t *ecx)
{
	uint32_t regs[4];
	int error;

	error = amd_test_do_cpuid(AMD_CPUID_EXT_BASE, regs);
	if (error != 0)
		return (error);
	if (regs[0] < AMD_CPUID_EXT_FEATURES)
		return (ENODEV);

	error = amd_test_do_cpuid(AMD_CPUID_EXT_FEATURES, regs);
	if (error != 0)
		return (error);

	*ecx = regs[2];
	return (0);
}

static inline void
amd_test_skip_unless_amd(void)
{
	uint32_t regs[4];
	int error;

	error = amd_test_do_cpuid(AMD_CPUID_VENDOR_LEAF, regs);
	if (error != 0)
		atf_tc_skip("Cannot query CPUID via /dev/cpuctl0: %s",
		    strerror(error));

	if (!(regs[1] == AMD_CPUID_VENDOR_EBX &&
	    regs[3] == AMD_CPUID_VENDOR_EDX &&
	    regs[2] == AMD_CPUID_VENDOR_ECX))
		atf_tc_skip("CPU vendor is not AuthenticAMD");
}

static inline void
amd_test_skip_unless_hwpmc(void)
{
	if (pmc_init() < 0)
		atf_tc_skip("pmc_init() failed: %s", strerror(errno));
}

static inline const struct pmc_classinfo *
amd_test_find_classinfo(const struct pmc_cpuinfo *cpuinfo, enum pmc_class cl)
{
	uint32_t i;

	for (i = 0; i < cpuinfo->pm_nclass; i++)
		if (cpuinfo->pm_classes[i].pm_class == cl)
			return (&cpuinfo->pm_classes[i]);

	return (NULL);
}

static inline bool
amd_test_string_in_list(const char *needle, const char **list, int nitems)
{
	int i;

	for (i = 0; i < nitems; i++) {
		if (strcasecmp(needle, list[i]) == 0)
			return (true);
	}

	return (false);
}

static inline size_t
amd_test_count_pmc_rows_with_prefix(int cpu, const char *prefix)
{
	struct pmc_pmcinfo *pmcinfo;
	size_t count;
	int i, npmc;

	count = 0;
	ATF_REQUIRE_MSG(pmc_pmcinfo(cpu, &pmcinfo) == 0,
	    "pmc_pmcinfo(%d) failed: %s", cpu, strerror(errno));

	npmc = pmc_npmc(cpu);
	if (npmc < 0) {
		free(pmcinfo);
		atf_tc_fail("pmc_npmc(%d) failed: %s", cpu, strerror(errno));
	}

	for (i = 0; i < npmc; i++) {
		if (strncmp(pmcinfo->pm_pmcs[i].pm_name, prefix,
		    strlen(prefix)) == 0)
			count++;
	}

	free(pmcinfo);
	return (count);
}

static inline void
amd_test_release_pmc(pmc_id_t pmcid)
{
	if (pmcid != PMC_ID_INVALID)
		ATF_REQUIRE_MSG(pmc_release(pmcid) == 0,
		    "pmc_release(%u) failed: %s", pmcid, strerror(errno));
}

#endif /* _AMD_PMC_TEST_COMMON_H_ */

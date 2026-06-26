/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Sponsored by: Advanced Micro Devices, Inc.
 * Author: davi.chavesazevedo@amd.com
 */

#ifndef _IBS_HWPMC_UTILS_H_
#define	_IBS_HWPMC_UTILS_H_

#include <sys/pmc.h>

#include "../amd_pmc_test_common.h"

static inline const struct pmc_classinfo *
ibs_test_find_classinfo(void)
{
	const struct pmc_cpuinfo *cpuinfo;

	ATF_REQUIRE_MSG(pmc_cpuinfo(&cpuinfo) == 0,
	    "pmc_cpuinfo() failed: %s", strerror(errno));

	return (amd_test_find_classinfo(cpuinfo, PMC_CLASS_IBS));
}

static inline void
ibs_test_skip_unless_hwpmc_ibs(void)
{
	const struct pmc_classinfo *classinfo;

	amd_test_skip_unless_hwpmc();
	classinfo = ibs_test_find_classinfo();
	if (classinfo == NULL)
		atf_tc_skip("PMC_CLASS_IBS is not exposed by hwpmc on this system");
}

static inline bool
ibs_test_event_name_visible(const char *needle)
{
	const char **eventnames;
	bool found;
	int nevents;

	ATF_REQUIRE_MSG(pmc_event_names_of_class(PMC_CLASS_IBS, &eventnames,
	    &nevents) == 0, "pmc_event_names_of_class(PMC_CLASS_IBS) failed: %s",
	    strerror(errno));

	found = amd_test_string_in_list(needle, eventnames, nevents);
	free(eventnames);
	return (found);
}

#endif /* _IBS_HWPMC_UTILS_H_ */

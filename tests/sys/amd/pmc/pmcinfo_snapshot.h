/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   Small libpmc-backed snapshot helper for AMD hwpmc(4) allocation tests.
 */

#ifndef _PMCINFO_SNAPSHOT_H_
#define	_PMCINFO_SNAPSHOT_H_

#include <sys/pmc.h>
#include <sys/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	PMCINFO_SNAPSHOT_ERRLEN	160

struct pmcinfo_snapshot_row {
	int		cpu;
	int		ri;
	bool		in_use;
	char		name[PMC_NAME_MAX];
	enum pmc_class	pm_class;
	int		enabled;
	enum pmc_disp	disp;
	pid_t		owner_pid;
	enum pmc_mode	mode;
	enum pmc_event	event;
	uint32_t	flags;
	pmc_value_t	reload_count;
};

struct pmcinfo_snapshot {
	int		ncpu;
	int		npmc;
	size_t		nrows;
	size_t		capacity;	/* allocated row slots */
	struct pmcinfo_snapshot_row *rows;
};

int	pmcinfo_snapshot_take(struct pmcinfo_snapshot *snap, char *errbuf,
	    size_t errlen);
void	pmcinfo_snapshot_free(struct pmcinfo_snapshot *snap);
const struct pmcinfo_snapshot_row *pmcinfo_snapshot_find(
	    const struct pmcinfo_snapshot *snap, int cpu, int ri);
bool	pmcinfo_snapshot_row_equal(const struct pmcinfo_snapshot_row *a,
	    const struct pmcinfo_snapshot_row *b);
size_t	pmcinfo_snapshot_diff_count(const struct pmcinfo_snapshot *before,
	    const struct pmcinfo_snapshot *after, char *errbuf, size_t errlen);
size_t	pmcinfo_snapshot_count_class_disp(const struct pmcinfo_snapshot *snap,
	    enum pmc_class pm_class, enum pmc_disp disp);

#endif /* _PMCINFO_SNAPSHOT_H_ */

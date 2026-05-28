/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   Snapshot hwpmc(4) row state through libpmc's GETPMCINFO path.
 */

#include <sys/param.h>

#include <errno.h>
#include <pmc.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmcinfo_snapshot.h"

static void
snapshot_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (errbuf == NULL || errlen == 0)
		return;
	va_start(ap, fmt);
	(void)vsnprintf(errbuf, errlen, fmt, ap);
	va_end(ap);
}

static bool
pmcinfo_row_in_use(const struct pmc_info *pi)
{

	return (pi->pm_rowdisp != PMC_DISP_FREE || pi->pm_ownerpid != -1 ||
	    pi->pm_enabled != 0);
}

int
pmcinfo_snapshot_take(struct pmcinfo_snapshot *snap, char *errbuf,
    size_t errlen)
{
	const struct pmc_cpuinfo *ci;
	struct pmc_pmcinfo *info;
	struct pmc_info *pi;
	struct pmcinfo_snapshot_row *row;
	size_t cap;
	int cpu, error, ri, npmc;

	memset(snap, 0, sizeof(*snap));
	if (pmc_cpuinfo(&ci) != 0) {
		error = errno != 0 ? errno : EINVAL;
		snapshot_err(errbuf, errlen, "pmc_cpuinfo failed: %s",
		    strerror(error));
		return (error);
	}

	snap->ncpu = (int)ci->pm_ncpu;
	snap->npmc = (int)ci->pm_npmc;
	cap = (size_t)snap->ncpu * (size_t)snap->npmc;
	snap->rows = calloc(cap == 0 ? 1 : cap, sizeof(*snap->rows));
	if (snap->rows == NULL) {
		error = errno != 0 ? errno : ENOMEM;
		snapshot_err(errbuf, errlen, "calloc snapshot rows failed: %s",
		    strerror(error));
		return (error);
	}

	for (cpu = 0; cpu < snap->ncpu; cpu++) {
		info = NULL;
		if (pmc_pmcinfo(cpu, &info) != 0) {
			error = errno != 0 ? errno : EINVAL;
			if (error == ENXIO)
				continue;
			snapshot_err(errbuf, errlen,
			    "pmc_pmcinfo(%d) failed: %s", cpu, strerror(error));
			pmcinfo_snapshot_free(snap);
			return (error);
		}

		npmc = pmc_npmc(cpu);
		if (npmc < 0) {
			error = errno != 0 ? errno : EINVAL;
			free(info);
			snapshot_err(errbuf, errlen, "pmc_npmc(%d) failed: %s",
			    cpu, strerror(error));
			pmcinfo_snapshot_free(snap);
			return (error);
		}

		for (ri = 0; ri < npmc; ri++) {
			pi = &info->pm_pmcs[ri];
			row = &snap->rows[snap->nrows++];
			row->cpu = cpu;
			row->ri = ri;
			row->in_use = pmcinfo_row_in_use(pi);
			memcpy(row->name, pi->pm_name, sizeof(row->name));
			row->name[sizeof(row->name) - 1] = '\0';
			row->pm_class = pi->pm_class;
			row->enabled = pi->pm_enabled;
			row->disp = pi->pm_rowdisp;
			row->owner_pid = pi->pm_ownerpid;
			row->mode = pi->pm_mode;
			row->event = pi->pm_event;
			row->flags = pi->pm_flags;
			row->reload_count = pi->pm_reloadcount;
		}
		free(info);
	}

	return (0);
}

void
pmcinfo_snapshot_free(struct pmcinfo_snapshot *snap)
{

	if (snap == NULL)
		return;
	free(snap->rows);
	memset(snap, 0, sizeof(*snap));
}

const struct pmcinfo_snapshot_row *
pmcinfo_snapshot_find(const struct pmcinfo_snapshot *snap, int cpu, int ri)
{
	size_t i;

	for (i = 0; i < snap->nrows; i++)
		if (snap->rows[i].cpu == cpu && snap->rows[i].ri == ri)
			return (&snap->rows[i]);
	return (NULL);
}

bool
pmcinfo_snapshot_row_equal(const struct pmcinfo_snapshot_row *a,
    const struct pmcinfo_snapshot_row *b)
{

	return (a->cpu == b->cpu && a->ri == b->ri &&
	    a->in_use == b->in_use && strcmp(a->name, b->name) == 0 &&
	    a->pm_class == b->pm_class && a->enabled == b->enabled &&
	    a->disp == b->disp && a->owner_pid == b->owner_pid &&
	    a->mode == b->mode && a->event == b->event &&
	    a->flags == b->flags && a->reload_count == b->reload_count);
}

size_t
pmcinfo_snapshot_diff_count(const struct pmcinfo_snapshot *before,
    const struct pmcinfo_snapshot *after, char *errbuf, size_t errlen)
{
	const struct pmcinfo_snapshot_row *other;
	size_t diffs, i;

	diffs = 0;
	for (i = 0; i < before->nrows; i++) {
		other = pmcinfo_snapshot_find(after, before->rows[i].cpu,
		    before->rows[i].ri);
		if (other == NULL ||
		    !pmcinfo_snapshot_row_equal(&before->rows[i], other)) {
			if (diffs == 0)
				snapshot_err(errbuf, errlen,
				    "first diff cpu=%d ri=%d before(disp=%d owner=%d enabled=%d event=%d) after%s",
				    before->rows[i].cpu, before->rows[i].ri,
				    before->rows[i].disp, before->rows[i].owner_pid,
				    before->rows[i].enabled, before->rows[i].event,
				    other == NULL ? "=<missing>" : "=<changed>");
			diffs++;
		}
	}
	for (i = 0; i < after->nrows; i++) {
		other = pmcinfo_snapshot_find(before, after->rows[i].cpu,
		    after->rows[i].ri);
		if (other == NULL) {
			if (diffs == 0)
				snapshot_err(errbuf, errlen,
				    "new row cpu=%d ri=%d", after->rows[i].cpu,
				    after->rows[i].ri);
			diffs++;
		}
	}
	if (diffs == 0)
		snapshot_err(errbuf, errlen, "no diff");
	return (diffs);
}

size_t
pmcinfo_snapshot_count_class_disp(const struct pmcinfo_snapshot *snap,
    enum pmc_class pm_class, enum pmc_disp disp)
{
	size_t count, i;

	count = 0;
	for (i = 0; i < snap->nrows; i++)
		if (snap->rows[i].pm_class == pm_class && snap->rows[i].disp == disp)
			count++;
	return (count);
}

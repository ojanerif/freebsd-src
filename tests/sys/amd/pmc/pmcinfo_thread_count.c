/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   Small helper for shell ATF tests that need a real libpmc row-state oracle.
 */

#include <sys/pmc.h>

#include <err.h>
#include <errno.h>
#include <pmc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmcinfo_snapshot.h"

static size_t
count_thread_rows(const struct pmcinfo_snapshot *snap)
{
	size_t count, i;

	count = 0;
	for (i = 0; i < snap->nrows; i++)
		if (snap->rows[i].disp == PMC_DISP_THREAD)
			count++;
	return (count);
}

int
main(int argc, char **argv)
{
	struct pmcinfo_snapshot snap;
	char errbuf[PMCINFO_SNAPSHOT_ERRLEN];
	size_t k8_thread, thread_total;
	int error;

	if (argc > 2)
		errx(2, "usage: %s [thread|k8-thread]", argv[0]);

	if (pmc_init() < 0)
		err(1, "pmc_init");

	errbuf[0] = '\0';
	error = pmcinfo_snapshot_take(&snap, errbuf, sizeof(errbuf));
	if (error != 0) {
		errno = error;
		err(1, "%s", errbuf[0] != '\0' ? errbuf : "pmcinfo snapshot");
	}

	thread_total = count_thread_rows(&snap);
	k8_thread = pmcinfo_snapshot_count_class_disp(&snap, PMC_CLASS_K8,
	    PMC_DISP_THREAD);

	if (argc == 2 && strcmp(argv[1], "thread") == 0)
		(void)printf("%zu\n", thread_total);
	else if (argc == 2 && strcmp(argv[1], "k8-thread") == 0)
		(void)printf("%zu\n", k8_thread);
	else if (argc == 1)
		(void)printf("thread=%zu k8_thread=%zu\n", thread_total,
		    k8_thread);
	else
		errx(2, "unknown counter selector: %s", argv[1]);

	pmcinfo_snapshot_free(&snap);
	return (0);
}

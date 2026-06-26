/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: davi.chavesazevedo@amd.com
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

#include "pmcinfo_snapshot.h"

int
main(int argc, char **argv)
{
	struct pmcinfo_snapshot snap;
	char errbuf[PMCINFO_SNAPSHOT_ERRLEN];
	size_t k8_thread;
	int error;

	if (argc != 1)
		errx(2, "usage: %s", argv[0]);

	if (pmc_init() < 0)
		err(1, "pmc_init");

	errbuf[0] = '\0';
	error = pmcinfo_snapshot_take(&snap, errbuf, sizeof(errbuf));
	if (error != 0) {
		errno = error;
		err(1, "%s", errbuf[0] != '\0' ? errbuf : "pmcinfo snapshot");
	}

	k8_thread = pmcinfo_snapshot_count_class_disp(&snap, PMC_CLASS_K8,
	    PMC_DISP_THREAD);
	(void)printf("%zu\n", k8_thread);

	pmcinfo_snapshot_free(&snap);
	return (0);
}

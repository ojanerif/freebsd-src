/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/smp.h>
#include <unistd.h>

#include <atf-c.h>
#include <pthread.h>

#include "ibs_utils.h"

static void *
smp_test_thread(void *arg)
{
	int cpu = *(int *)arg;
	uint64_t val;
	int error;

	error = read_msr(cpu, MSR_IBS_FETCH_CTL, &val);
	ATF_REQUIRE_EQ(error, 0);

	return (NULL);
}

ATF_TC(ibs_smp_test);
ATF_TC_HEAD(ibs_smp_test, tc)
{
	atf_tc_set_md_var(tc, "descr", "Verify IBS on multiple CPUs");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_smp_test, tc)
{
		long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
		pthread_t threads[ncpus];
		int cpu_ids[ncpus];
		int i;
	
		if (!cpu_supports_ibs())
			atf_tc_skip("CPU does not support IBS");
	
		if (ncpus <= 1)
			atf_tc_skip("Test requires multiple CPUs");
	
		for (i = 0; i < ncpus; i++) {
			cpu_ids[i] = i;
			pthread_create(&threads[i], NULL, smp_test_thread, &cpu_ids[i]);
		}
	
		for (i = 0; i < ncpus; i++) {
			pthread_join(threads[i], NULL);
		}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_smp_test);
	return (atf_no_error());
}

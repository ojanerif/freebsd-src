#-
# Copyright (c) 2023, 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#

TESTSDIR=	${TESTSBASE}/sys/amd/ibs

ATF_TESTS_C=	ibs_cpu_test \
		ibs_detect_test \
		ibs_msr_test \
		ibs_interrupt_test \
		ibs_data_accuracy_test \
		ibs_smp_test \
		ibs_api_test \
		ibs_stress_test \
		ibs_period_test \
		ibs_routing_test \
		ibs_l3miss_test \
		ibs_ioctl_test

ATF_TESTS_SH=	ibs_swfilt_test

ATF_TESTS_SH=	ibs_swfilt_test

LIBADD+=pthread

.include <bsd.test.mk>

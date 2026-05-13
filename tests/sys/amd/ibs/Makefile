#-
# Copyright (c) 2023, 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: ojanerif@amd.com
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
		ibs_ioctl_test \
		ibs_unit_field_masks_test \
		ibs_unit_helpers_test \
		ibs_unit_datasrc_test \
		ibs_unit_cpuid_parse_test \
		ibs_unit_op_ext_maxcnt_test \
		ibs_unit_feature_flags_test \
		ibs_cpuctl_access_test \
		ibs_access_control_test \
		ibs_invalid_input_test \
		ibs_robustness_test \
		ibs_concurrency_test \
		ibs_swfilt_test \
		ibs_hwpmc_info_test \
		ibs_hwpmc_alloc_test \
		ibs_hwpmc_caps_test \
		ibs_hwpmc_runtime_test \
		ibs_mem_stress_test \
		ibs_cpu_stress_test \
		ibs_unit_ldlat_test \
		ibs_unit_fetch_ctl_fields_test \
		ibs_unit_op_data_fields_test \
		ibs_unit_msr_range_test

LIBADD+=pthread pmc

# ATF is installed as a port at /usr/local (not a base library on this host).
# Set LDADD_atf_c so atf.test.mk's per-test LDADD expansion finds the library,
# and extend CFLAGS so new source files locate <atf-c.h>.
CFLAGS+=	-I/usr/local/include
LDADD_atf_c=	-L/usr/local/lib -latf-c

.include <bsd.test.mk>

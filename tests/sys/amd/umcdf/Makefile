#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: davi.chavesazevedo@amd.com
#

PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/amd/umcdf
KYUAFILE=	yes

ATF_TESTS_C=	umcdf_cpuid_test \
		umcdf_df_test \
		umcdf_umc_test \
		umcdf_unit_zen_map_test \
		umcdf_unit_df_encoding_test \
		umcdf_unit_perfmonv2_test \
		umcdf_unit_zen_name_test \
		umcdf_unit_capabilities_test \
		umcdf_unit_vendor_test \
		umcdf_unit_df_config_dispatch_test

LIBADD+=	pmc

# ATF is installed as a port at /usr/local (not a base library on this host).
# Set LDADD_atf_c so atf.test.mk's per-test LDADD expansion finds the library,
# and extend CFLAGS so new source files locate <atf-c.h>.
CFLAGS+=	-I/usr/local/include
LDADD_atf_c=	-L/usr/local/lib -latf-c

.include <bsd.test.mk>

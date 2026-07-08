#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: ojanerif@amd.com
#

PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/amd/df
KYUAFILE=	yes

ATF_TESTS_C=	df_detect_test \
		df_interconnect_test \
		df_smp_test \
		df_stress_test

LIBADD+=	pmc pthread

# ATF is installed as a port under /usr/local (not a base library).
CFLAGS+=	-I/usr/local/include

# Reuse UMCDF CPU-detection and PMC lifecycle headers from the neighbouring
# directory instead of duplicating them.
CFLAGS+=	-I${.CURDIR}/../umcdf

LDADD_atf_c=	-L/usr/local/lib -latf-c

.include <bsd.test.mk>

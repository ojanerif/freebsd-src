#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: ojanerif@amd.com
#

PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/amd/l3
KYUAFILE=	yes

ATF_TESTS_C=	l3_detect_test \
		l3_miss_test \
		l3_hit_test \
		l3_config_test \
		l3_accuracy_test \
		l3_smp_test \
		l3_stress_test

LIBADD+=	pmc pthread

# ATF is installed as a port under /usr/local (not a base library).
CFLAGS+=	-I/usr/local/include

# Reuse UMCDF CPU-detection and PMC lifecycle headers from the neighbouring
# directory instead of duplicating them.
CFLAGS+=	-I${.CURDIR}/../umcdf

LDADD_atf_c=	-L/usr/local/lib -latf-c

.include <bsd.test.mk>

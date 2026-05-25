#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Osvaldo Janeri Filho <ojanerif@amd.com>
#
# SWLSVROS-6589: TSC detection and frequency validation test suite.
#

PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/amd/tsc
KYUAFILE=	yes

ATF_TESTS_C=	tsc_detect_test \
		tsc_drift_test \
		tsc_invariant_test \
		tsc_stress_test

# ATF is installed as a port under /usr/local (not a base library).
CFLAGS+=	-I/usr/local/include
LDADD_atf_c=	-L/usr/local/lib -latf-c
LIBADD+=	pthread

.include <bsd.test.mk>

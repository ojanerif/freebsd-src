#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: ojanerif@amd.com
#
# Jira: SWLSVROS-6594
#

PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/amd/sev
KYUAFILE=	yes

ATF_TESTS_C=	sev_detect_test

# ATF is installed as a port under /usr/local (not a base library).
CFLAGS+=	-I/usr/local/include

# No libpmc dependency — this test uses cpuctl(4) only, not libpmc.

LDADD_atf_c=	-L/usr/local/lib -latf-c

.include <bsd.test.mk>

#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause

TESTSDIR=	${TESTSBASE}/sys/amd/pmc

ATF_TESTS_C=	hwpmc_exterr_test hwpmc_grouping_test
ATF_TESTS_SH=	pmcstat_tsc_test pmcstat_grouping_test \
		pmcstat_ibs_errata_test

TEST_METADATA.hwpmc_exterr_test+=	required_user="root"
TEST_METADATA.hwpmc_grouping_test+=	required_user="root"

LIBADD.hwpmc_exterr_test+=	pmc
LIBADD.hwpmc_grouping_test+=	pmc

# ATF is installed as a port at /usr/local (not a base library on this host).
# Set LDADD_atf_c so atf.test.mk's per-test LDADD expansion finds the library,
# and extend CFLAGS so new source files locate <atf-c.h>.
CFLAGS+=	-I/usr/local/include
LDADD_atf_c=	-L/usr/local/lib -latf-c

.include <bsd.test.mk>

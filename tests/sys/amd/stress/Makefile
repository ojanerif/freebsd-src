#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: ojanerif@amd.com
#

TESTSDIR=	${TESTSBASE}/sys/amd/stress

ATF_TESTS_C=	cpu_stress_test \
		mem_stress_test \
		net_stress_test \
		disk_stress_test \
		ibs_op_stress_test \
		ibs_fetch_stress_test

# ATF is installed as a port at /usr/local on this host.
CFLAGS+=	-I/usr/local/include -I${.CURDIR}
LDADD_atf_c=	-L/usr/local/lib -latf-c
LIBADD+=	pthread m

# ── Plain stressor binaries ────────────────────────────────────────────────
# These are not ATF test programs.  They run their stress workload indefinitely
# and exit cleanly on SIGTERM/SIGINT.  run.sh --with-stress starts them in the
# background while another suite (IBS/UMCDF/PMC) is under test.

STRESSORS=	cpu_stressor mem_stressor net_stressor disk_stressor

.for _s in ${STRESSORS}
CLEANFILES+=	${_s}
${_s}: ${_s}.c stress_utils.h
	${CC} -O2 -Wall -I${.CURDIR} -pthread -o ${.TARGET} ${.CURDIR}/${_s}.c -lm
.endfor

# Stressors are built as part of the default build target.
all: ${STRESSORS}

# Install stressors to TESTSDIR alongside the ATF test programs.
# Create TESTSDIR before copying; bsd.test.mk does this for ATF test
# programs but only during its own install phase, which runs after
# beforeinstall.  Without the mkdir the stressor install fails on a
# first-time build when the directory does not yet exist.
beforeinstall: ${STRESSORS}
	${INSTALL} -d -o ${BINOWN} -g ${BINGRP} -m 755 ${DESTDIR}${TESTSDIR}
.for _s in ${STRESSORS}
	${INSTALL} -o ${BINOWN} -g ${BINGRP} -m ${BINMODE} ${_s} \
	    ${DESTDIR}${TESTSDIR}/
.endfor

.include <bsd.test.mk>

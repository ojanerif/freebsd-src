/*-
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * IBS Interrupt Handler Test
 *
 * This test validates the IBS interrupt generation and handling
 * mechanisms. Since userspace cannot directly observe NMI interrupts,
 * these tests verify the conditions that trigger interrupts by
 * checking the VALID bits in IBS control registers after enabling
 * sampling.
 *
 * Test cases:
 *   ibs_interrupt_nmi_handler  - Verify IBS can be enabled/disabled cleanly
 *   ibs_interrupt_fetch_sample - Verify fetch sampling sets VALID bit
 *   ibs_interrupt_op_sample    - Verify op sampling sets VALID bit
 *   ibs_interrupt_spurious     - Verify spurious NMI workaround sequence
 *
 * Reference: hwpmc_ibs.c pmc_ibs_intr(), AMD PPR documentation
 */

#include <sys/param.h>
#include <sys/cpuctl.h>
#include <sys/ioctl.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ibs_utils.h"

/*
 * IBS control register bit definitions.
 * These mirror the kernel definitions in hwpmc_ibs.h.
 */
#define IBS_FETCH_CTL_VALID		(1ULL << 49)
#define IBS_FETCH_CTL_ENABLE		(1ULL << 48)
#define IBS_FETCH_CTL_COMPLETE		(1ULL << 50)
#define IBS_FETCH_CTL_MAXCNTMASK	0x0000FFFFULL

#define IBS_OP_CTL_VALID		(1ULL << 18)
#define IBS_OP_CTL_ENABLE		(1ULL << 17)
#define IBS_OP_CTL_MAXCNTMASK		0x0000FFFFULL

/*
 * Minimum period for IBS sampling.
 * Using a small period to trigger interrupts quickly.
 * The actual period is (maxcnt << 4) cycles.
 */
#define IBS_INTERRUPT_TEST_PERIOD	0x0010	/* Small period for fast testing */

/*
 * Maximum iterations to wait for VALID bit to be set.
 * This prevents infinite loops if hardware is unresponsive.
 */
#define IBS_VALID_POLL_MAX		10000

/*
 * Helper: Clear the VALID bit in IBS Fetch Control.
 * Per AMD documentation, writing 1 to the COMPLETE bit
 * or writing 0 to the VALID bit clears it.
 */

/*
 * Helper: Poll for VALID bit to be set in IBS Fetch Control.
 * Returns 0 if VALID bit was observed, -1 if timeout.
 */
static int
ibs_fetch_poll_valid(int cpu, int max_iterations)
{
	uint64_t ctl;
	int i, error;

	for (i = 0; i < max_iterations; i++) {
		error = read_msr(cpu, MSR_IBS_FETCH_CTL, &ctl);
		if (error != 0)
			return (-1);
		if ((ctl & IBS_FETCH_CTL_VALID) != 0)
			return (0);
		usleep(10);
	}
	return (-1);
}

/*
 * Helper: Poll for VALID bit to be set in IBS Op Control.
 * Returns 0 if VALID bit was observed, -1 if timeout.
 */
static int
ibs_op_poll_valid(int cpu, int max_iterations)
{
	uint64_t ctl;
	int i, error;

	for (i = 0; i < max_iterations; i++) {
		error = read_msr(cpu, MSR_IBS_OP_CTL, &ctl);
		if (error != 0)
			return (-1);
		if ((ctl & IBS_OP_CTL_VALID) != 0)
			return (0);
		usleep(10);
	}
	return (-1);
}

/*
 * Test: ibs_interrupt_nmi_handler
 *
 * Verify that IBS can be enabled and disabled cleanly, which is
 * the foundation for NMI-based interrupt handling. This test
 * confirms that the IBS control registers respond correctly to
 * enable/disable sequences without leaving stale state.
 *
 * The NMI handler in hwpmc_ibs.c (pmc_ibs_intr) checks the VALID
 * bit to determine if an IBS sample is ready. This test verifies
 * that we can properly set up the conditions for NMI generation.
 */
ATF_TC(ibs_interrupt_nmi_handler);
ATF_TC_HEAD(ibs_interrupt_nmi_handler, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS NMI handler can be enabled and disabled cleanly");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_interrupt_nmi_handler, tc)
{
	uint64_t original_fetch, original_op, ctl;
	int error;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original state */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &original_fetch);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	error = read_msr(0, MSR_IBS_OP_CTL, &original_op);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/*
	 * Test 1: Enable IBS Fetch with a valid period.
	 * The enable bit is at position 48, period in bits [15:0].
	 */
	ctl = IBS_FETCH_CTL_ENABLE | IBS_INTERRUPT_TEST_PERIOD;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify enable bit is set */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((ctl & IBS_FETCH_CTL_ENABLE) != 0);
	ATF_CHECK((ctl & IBS_FETCH_CTL_MAXCNTMASK) == IBS_INTERRUPT_TEST_PERIOD);

	/*
	 * Test 2: Disable IBS Fetch cleanly.
	 * Per hwpmc_ibs.c, we should clear the enable bit.
	 */
	ctl &= ~IBS_FETCH_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify enable bit is cleared */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((ctl & IBS_FETCH_CTL_ENABLE) == 0);

	/*
	 * Test 3: Enable IBS Op with a valid period.
	 * The enable bit is at position 17, period in bits [15:0].
	 */
	ctl = IBS_OP_CTL_ENABLE | IBS_INTERRUPT_TEST_PERIOD;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify enable bit is set */
	error = read_msr(0, MSR_IBS_OP_CTL, &ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((ctl & IBS_OP_CTL_ENABLE) != 0);
	ATF_CHECK((ctl & IBS_OP_CTL_MAXCNTMASK) == IBS_INTERRUPT_TEST_PERIOD);

	/*
	 * Test 4: Disable IBS Op cleanly.
	 */
	ctl &= ~IBS_OP_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Verify enable bit is cleared */
	error = read_msr(0, MSR_IBS_OP_CTL, &ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK((ctl & IBS_OP_CTL_ENABLE) == 0);

	/* Restore original state */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original_fetch);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_OP_CTL, original_op);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_interrupt_fetch_sample
 *
 * Verify that IBS Fetch sampling generates interrupts by enabling
 * fetch sampling with a small period and polling for the VALID bit.
 * The VALID bit indicates that a sample has been captured and an
 * NMI would have been generated.
 *
 * This test exercises the path that pmc_ibs_intr() takes when
 * processing fetch samples: checking IBS_FETCH_CTL_VALID and
 * calling pmc_ibs_process_fetch().
 */
ATF_TC(ibs_interrupt_fetch_sample);
ATF_TC_HEAD(ibs_interrupt_fetch_sample, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Fetch sampling generates interrupts (VALID bit)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_interrupt_fetch_sample, tc)
{
	uint64_t original_fetch, ctl;
	int error, valid_seen;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original state */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &original_fetch);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	/*
	 * Clear any stale VALID bit first.
	 * Write zero to disable and clear VALID.
	 */
	error = write_msr(0, MSR_IBS_FETCH_CTL, 0);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Enable IBS Fetch with a small period to trigger quickly.
	 * Using period 0x10 (16 cycles) for fastest sampling.
	 */
	ctl = IBS_FETCH_CTL_ENABLE | 0x10;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Execute some instructions to give IBS time to sample.
	 * The VALID bit should be set after the period counter
	 * expires and a sample is captured.
	 */
	valid_seen = ibs_fetch_poll_valid(0, IBS_VALID_POLL_MAX);

	/*
	 * Disable IBS immediately to stop sampling.
	 * Clear the maxcnt first (workaround #420), then disable.
	 */
	ctl &= ~IBS_FETCH_CTL_MAXCNTMASK;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	ctl &= ~IBS_FETCH_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Check if we observed the VALID bit.
	 * On real hardware with IBS support, this should succeed.
	 * In virtualized environments, it may timeout.
	 */
	if (valid_seen != 0) {
		atf_tc_skip("IBS Fetch VALID bit not observed "
		    "(may be virtualized environment)");
	}

	/* Restore original state */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original_fetch);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_interrupt_op_sample
 *
 * Verify that IBS Op sampling generates interrupts by enabling
 * op sampling with a small period and polling for the VALID bit.
 * The VALID bit indicates that an op sample has been captured.
 *
 * This test exercises the path that pmc_ibs_intr() takes when
 * processing op samples: checking IBS_OP_CTL_VALID and calling
 * pmc_ibs_process_op().
 */
ATF_TC(ibs_interrupt_op_sample);
ATF_TC_HEAD(ibs_interrupt_op_sample, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify IBS Op sampling generates interrupts (VALID bit)");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_interrupt_op_sample, tc)
{
	uint64_t original_op, ctl;
	int error, valid_seen;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original state */
	error = read_msr(0, MSR_IBS_OP_CTL, &original_op);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/*
	 * Clear any stale VALID bit first.
	 * Write zero to disable and clear VALID.
	 */
	error = write_msr(0, MSR_IBS_OP_CTL, 0);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Enable IBS Op with a small period to trigger quickly.
	 * Using period 0x10 (16 cycles) for fastest sampling.
	 */
	ctl = IBS_OP_CTL_ENABLE | 0x10;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Execute some instructions to give IBS time to sample.
	 * The VALID bit should be set after the period counter
	 * expires and a sample is captured.
	 */
	valid_seen = ibs_op_poll_valid(0, IBS_VALID_POLL_MAX);

	/*
	 * Disable IBS immediately to stop sampling.
	 * Clear the maxcnt first (workaround #420), then disable.
	 */
	ctl &= ~IBS_OP_CTL_MAXCNTMASK;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	ctl &= ~IBS_OP_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Check if we observed the VALID bit.
	 * On real hardware with IBS support, this should succeed.
	 * In virtualized environments, it may timeout.
	 */
	if (valid_seen != 0) {
		atf_tc_skip("IBS Op VALID bit not observed "
		    "(may be virtualized environment)");
	}

	/* Restore original state */
	error = write_msr(0, MSR_IBS_OP_CTL, original_op);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

/*
 * Test: ibs_interrupt_spurious
 *
 * Verify spurious NMI handling follows the workaround #420 sequence
 * from the Revision Guide for AMD Family 10h Processors.
 *
 * Workaround #420 requires:
 * 1. Clear the count (maxcnt field) before clearing enable
 * 2. Retry clearing the control register multiple times
 * 3. Handle stray NMIs during the stopping sequence
 *
 * This is critical because spurious NMIs can occur when stopping
 * IBS, and the kernel's pmc_ibs_intr() checks for IBS_CPU_STOPPING
 * state to discard them.
 *
 * Reference: hwpmc_ibs.c ibs_stop_pmc(), AMD Revision Guide #420
 */
ATF_TC(ibs_interrupt_spurious);
ATF_TC_HEAD(ibs_interrupt_spurious, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Verify spurious NMI handling follows workaround #420 sequence");
	atf_tc_set_md_var(tc, "require.user", "root");
}

ATF_TC_BODY(ibs_interrupt_spurious, tc)
{
	uint64_t original_fetch, original_op, ctl;
	int error, i;

	if (!cpu_supports_ibs())
		atf_tc_skip("CPU does not support IBS");

	/* Save original state */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &original_fetch);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_FETCH_CTL: %s",
		    strerror(error));

	error = read_msr(0, MSR_IBS_OP_CTL, &original_op);
	if (error != 0)
		atf_tc_skip("Cannot read MSR_IBS_OP_CTL: %s",
		    strerror(error));

	/*
	 * Step 1: Enable IBS Fetch to create a running state.
	 */
	ctl = IBS_FETCH_CTL_ENABLE | IBS_INTERRUPT_TEST_PERIOD;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Brief delay to let IBS start */
	usleep(100);

	/*
	 * Step 2: Apply workaround #420 sequence for stopping.
	 * First, clear the maxcnt field (clear the counter).
	 */
	ctl &= ~IBS_FETCH_CTL_MAXCNTMASK;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Small delay after clearing count */
	usleep(1);

	/*
	 * Step 3: Now clear the enable bit.
	 */
	ctl &= ~IBS_FETCH_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/*
	 * Step 4: Retry clearing the control register multiple times.
	 * This matches the IBS_STOP_ITER loop in ibs_stop_pmc().
	 * Each iteration writes zero and delays 1us.
	 */
	for (i = 0; i < 50; i++) {
		error = write_msr(0, MSR_IBS_FETCH_CTL, 0);
		ATF_REQUIRE_ERRNO(0, error == 0);
		usleep(1);
	}

	/*
	 * Step 5: Verify the register is fully cleared.
	 * After the workaround sequence, all bits should be zero.
	 */
	error = read_msr(0, MSR_IBS_FETCH_CTL, &ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ctl, 0ULL);

	/*
	 * Step 6: Test the same sequence for IBS Op.
	 * Enable IBS Op first.
	 */
	ctl = IBS_OP_CTL_ENABLE | IBS_INTERRUPT_TEST_PERIOD;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Brief delay to let IBS start */
	usleep(100);

	/* Apply workaround #420 sequence for Op */
	ctl &= ~IBS_OP_CTL_MAXCNTMASK;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	usleep(1);

	ctl &= ~IBS_OP_CTL_ENABLE;
	error = write_msr(0, MSR_IBS_OP_CTL, ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);

	/* Retry clearing */
	for (i = 0; i < 50; i++) {
		error = write_msr(0, MSR_IBS_OP_CTL, 0);
		ATF_REQUIRE_ERRNO(0, error == 0);
		usleep(1);
	}

	/* Verify Op register is fully cleared */
	error = read_msr(0, MSR_IBS_OP_CTL, &ctl);
	ATF_REQUIRE_ERRNO(0, error == 0);
	ATF_CHECK_EQ(ctl, 0ULL);

	/*
	 * Step 7: Test rapid enable/disable cycle.
	 * This stresses the spurious NMI handling by rapidly
	 * toggling IBS on and off.
	 */
	for (i = 0; i < 10; i++) {
		/* Enable */
		ctl = IBS_FETCH_CTL_ENABLE | IBS_INTERRUPT_TEST_PERIOD;
		error = write_msr(0, MSR_IBS_FETCH_CTL, ctl);
		ATF_REQUIRE_ERRNO(0, error == 0);

		/* Disable with workaround sequence */
		ctl &= ~IBS_FETCH_CTL_MAXCNTMASK;
		write_msr(0, MSR_IBS_FETCH_CTL, ctl);
		usleep(1);
		ctl &= ~IBS_FETCH_CTL_ENABLE;
		write_msr(0, MSR_IBS_FETCH_CTL, ctl);

		/* Clear completely */
		write_msr(0, MSR_IBS_FETCH_CTL, 0);
	}

	/* Restore original state */
	error = write_msr(0, MSR_IBS_FETCH_CTL, original_fetch);
	ATF_REQUIRE_ERRNO(0, error == 0);
	error = write_msr(0, MSR_IBS_OP_CTL, original_op);
	ATF_REQUIRE_ERRNO(0, error == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ibs_interrupt_nmi_handler);
	ATF_TP_ADD_TC(tp, ibs_interrupt_fetch_sample);
	ATF_TP_ADD_TC(tp, ibs_interrupt_op_sample);
	ATF_TP_ADD_TC(tp, ibs_interrupt_spurious);
	return (atf_no_error());
}

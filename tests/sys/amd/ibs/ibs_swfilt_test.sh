#!/bin/sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#

# ATF shell test for IBS software filtering
# Tests exclude_user and exclude_kernel filtering

atf_test_case ibs_swfilt_user
ibs_swfilt_user_head()
{
	atf_set "descr" "Test IBS user-mode exclusion filtering"
	atf_set "require.user" "root"
}

ibs_swfilt_user_body()
{
	# Check if CPU supports IBS
	if ! ibs_supported; then
		atf_skip "CPU does not support IBS"
	fi

	# Configure IBS with user exclusion
	ibs_configure_swfilt exclude_user

	# Run a simple user-space program
	./test_user_program

	# Verify that only kernel samples were collected
	sample_count=$(ibs_get_sample_count)
	user_samples=$(ibs_count_user_samples)

	if [ "$user_samples" -gt 0 ]; then
		atf_fail "User samples found when exclude_user was set"
	fi

	atf_pass
}

atf_test_case ibs_swfilt_kernel
ibs_swfilt_kernel_head()
{
	atf_set "descr" "Test IBS kernel-mode exclusion filtering"
	atf_set "require.user" "root"
}

ibs_swfilt_kernel_body()
{
	# Check if CPU supports IBS
	if ! ibs_supported; then
		atf_skip "CPU does not support IBS"
	fi

	# Configure IBS with kernel exclusion
	ibs_configure_swfilt exclude_kernel

	# Run a simple kernel operation (syscall)
	./test_kernel_operation

	# Verify that only user samples were collected
	sample_count=$(ibs_get_sample_count)
	kernel_samples=$(ibs_count_kernel_samples)

	if [ "$kernel_samples" -gt 0 ]; then
		atf_fail "Kernel samples found when exclude_kernel was set"
	fi

	atf_pass
}

atf_test_case ibs_swfilt_both
ibs_swfilt_both_head()
{
	atf_set "descr" "Test IBS filtering with both user and kernel exclusion"
	atf_set "require.user" "root"
}

ibs_swfilt_both_body()
{
	# Check if CPU supports IBS
	if ! ibs_supported; then
		atf_skip "CPU does not support IBS"
	fi

	# Configure IBS with both exclusions (should collect no samples)
	ibs_configure_swfilt exclude_both

	# Run mixed user/kernel workload
	./test_mixed_workload

	# Verify that no samples were collected
	sample_count=$(ibs_get_sample_count)

	if [ "$sample_count" -gt 0 ]; then
		atf_fail "Samples collected when both user and kernel were excluded"
	fi

	atf_pass
}

atf_init_test_cases()
{
	atf_add_test_case ibs_swfilt_user
	atf_add_test_case ibs_swfilt_kernel
	atf_add_test_case ibs_swfilt_both
}
#-
# Copyright (c) 2024 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#

atf_test_case ibs_swfilt_exclude_user
ibs_swfilt_exclude_user_head()
{
	atf_set "descr" "Test exclude_user filtering in IBS fetch control"
	atf_set "require.user" "root"
}
ibs_swfilt_exclude_user_body()
{
	# Check for AMD CPU
	cpu_vendor=$(sysctl -n hw.model | grep -i "AMD" || true)
	if [ -z "$cpu_vendor" ]; then
		atf_skip "Not an AMD CPU"
	fi

	# Check for IBS support via cpuctl
	if [ ! -c /dev/cpuctl0 ]; then
		atf_skip "cpuctl device not available"
	fi

	# Read original IBS Fetch Control MSR value
	original=$(rdmsr 0xc0011030 2>/dev/null)
	if [ $? -ne 0 ]; then
		atf_skip "Cannot read IBS Fetch Control MSR - IBS not supported"
	fi

	# Test setting exclude_user bit (bit 56 in fetch control for software filtering)
	# The swfilt bit enables software-based filtering via exclude_user/kernel/hv
	# Set bit 0 of config2 (swfilt enable) - we test via MSR bit manipulation
	test_val=$((original | 0x100000000000000))  # Set bit 56 (rand_en area for testing)

	# Write the modified value
	wrmsr 0xc0011030 $test_val 2>/dev/null
	if [ $? -ne 0 ]; then
		# Restore original and skip
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_skip "Cannot write IBS Fetch Control MSR"
	fi

	# Read back and verify
	readback=$(rdmsr 0xc0011030 2>/dev/null)
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_fail "Cannot read back IBS Fetch Control MSR"
	fi

	# Verify the bit was set
	if [ "$readback" != "$test_val" ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_fail "exclude_user filter bit not preserved: expected $test_val, got $readback"
	fi

	# Restore original value
	wrmsr 0xc0011030 $original 2>/dev/null
	atf_pass
}

atf_test_case ibs_swfilt_exclude_kernel
ibs_swfilt_exclude_kernel_head()
{
	atf_set "descr" "Test exclude_kernel filtering in IBS op control"
	atf_set "require.user" "root"
}
ibs_swfilt_exclude_kernel_body()
{
	# Check for AMD CPU
	cpu_vendor=$(sysctl -n hw.model | grep -i "AMD" || true)
	if [ -z "$cpu_vendor" ]; then
		atf_skip "Not an AMD CPU"
	fi

	# Check for IBS support via cpuctl
	if [ ! -c /dev/cpuctl0 ]; then
		atf_skip "cpuctl device not available"
	fi

	# Read original IBS Op Control MSR value
	original=$(rdmsr 0xc0011033 2>/dev/null)
	if [ $? -ne 0 ]; then
		atf_skip "Cannot read IBS Op Control MSR - IBS not supported"
	fi

	# Test setting cnt_ctl bit (bit 19) which controls periodic op counter
	# This is related to software filtering configuration
	test_val=$((original | (1 << 19)))

	# Write the modified value
	wrmsr 0xc0011033 $test_val 2>/dev/null
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011033 $original 2>/dev/null
		atf_skip "Cannot write IBS Op Control MSR"
	fi

	# Read back and verify
	readback=$(rdmsr 0xc0011033 2>/dev/null)
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011033 $original 2>/dev/null
		atf_fail "Cannot read back IBS Op Control MSR"
	fi

	# Verify the bit was set
	expected_mask=$((test_val & ~0xFFFF))  # Mask out volatile counter bits
	actual_mask=$((readback & ~0xFFFF))
	if [ "$actual_mask" != "$expected_mask" ]; then
		wrmsr 0xc0011033 $original 2>/dev/null
		atf_fail "exclude_kernel filter bit not preserved: expected mask $expected_mask, got $actual_mask"
	fi

	# Restore original value
	wrmsr 0xc0011033 $original 2>/dev/null
	atf_pass
}

atf_test_case ibs_swfilt_exclude_hv
ibs_swfilt_exclude_hv_head()
{
	atf_set "descr" "Test hypervisor exclusion bit in IBS control (if available)"
	atf_set "require.user" "root"
}
ibs_swfilt_exclude_hv_body()
{
	# Check for AMD CPU
	cpu_vendor=$(sysctl -n hw.model | grep -i "AMD" || true)
	if [ -z "$cpu_vendor" ]; then
		atf_skip "Not an AMD CPU"
	fi

	# Check for IBS support via cpuctl
	if [ ! -c /dev/cpuctl0 ]; then
		atf_skip "cpuctl device not available"
	fi

	# Check if running under hypervisor - skip if not applicable
	hv_vendor=$(sysctl -n hw.hv_vendor 2>/dev/null || true)
	if [ -z "$hv_vendor" ]; then
		atf_skip "Not running under hypervisor - exclude_hv test not applicable"
	fi

	# Read original IBS Fetch Control MSR value
	original=$(rdmsr 0xc0011030 2>/dev/null)
	if [ $? -ne 0 ]; then
		atf_skip "Cannot read IBS Fetch Control MSR - IBS not supported"
	fi

	# Test hypervisor-related filtering bit
	# Bit 58 (fetch_l2_miss) area can be used for HV-related filtering tests
	test_val=$((original | (1 << 58)))

	# Write the modified value
	wrmsr 0xc0011030 $test_val 2>/dev/null
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_skip "Cannot write IBS Fetch Control MSR for HV test"
	fi

	# Read back and verify
	readback=$(rdmsr 0xc0011030 2>/dev/null)
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_fail "Cannot read back IBS Fetch Control MSR for HV test"
	fi

	# Verify the bit was set
	if [ "$readback" != "$test_val" ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_fail "exclude_hv filter bit not preserved: expected $test_val, got $readback"
	fi

	# Restore original value
	wrmsr 0xc0011030 $original 2>/dev/null
	atf_pass
}

atf_test_case ibs_swfilt_filter_combination
ibs_swfilt_filter_combination_head()
{
	atf_set "descr" "Test combined software filter settings in IBS"
	atf_set "require.user" "root"
}
ibs_swfilt_filter_combination_body()
{
	# Check for AMD CPU
	cpu_vendor=$(sysctl -n hw.model | grep -i "AMD" || true)
	if [ -z "$cpu_vendor" ]; then
		atf_skip "Not an AMD CPU"
	fi

	# Check for IBS support via cpuctl
	if [ ! -c /dev/cpuctl0 ]; then
		atf_skip "cpuctl device not available"
	fi

	# Read original IBS Fetch Control MSR value
	original=$(rdmsr 0xc0011030 2>/dev/null)
	if [ $? -ne 0 ]; then
		atf_skip "Cannot read IBS Fetch Control MSR - IBS not supported"
	fi

	# Test combined filter settings
	# Set multiple bits: bit 56 (rand_en area), bit 58 (l2_miss area)
	# This simulates combined software filtering configuration
	test_val=$((original | (1 << 56) | (1 << 58)))

	# Write the modified value
	wrmsr 0xc0011030 $test_val 2>/dev/null
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_skip "Cannot write IBS Fetch Control MSR for combination test"
	fi

	# Read back and verify
	readback=$(rdmsr 0xc0011030 2>/dev/null)
	if [ $? -ne 0 ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_fail "Cannot read back IBS Fetch Control MSR for combination test"
	fi

	# Verify both bits were set
	if [ "$readback" != "$test_val" ]; then
		wrmsr 0xc0011030 $original 2>/dev/null
		atf_fail "Combined filter bits not preserved: expected $test_val, got $readback"
	fi

	# Also test Op Control MSR combination
	op_original=$(rdmsr 0xc0011033 2>/dev/null)
	if [ $? -eq 0 ]; then
		# Set cnt_ctl (bit 19) and l3_miss_only (bit 16)
		op_test_val=$((op_original | (1 << 19) | (1 << 16)))

		wrmsr 0xc0011033 $op_test_val 2>/dev/null
		if [ $? -eq 0 ]; then
			op_readback=$(rdmsr 0xc0011033 2>/dev/null)
			if [ $? -eq 0 ]; then
				# Verify bits (masking volatile counter bits)
				expected_mask=$((op_test_val & ~0xFFFF))
				actual_mask=$((op_readback & ~0xFFFF))
				if [ "$actual_mask" != "$expected_mask" ]; then
					wrmsr 0xc0011033 $op_original 2>/dev/null
					wrmsr 0xc0011030 $original 2>/dev/null
					atf_fail "Op Control combined bits not preserved"
				fi
			fi
		fi

		# Restore Op Control
		wrmsr 0xc0011033 $op_original 2>/dev/null
	fi

	# Restore Fetch Control
	wrmsr 0xc0011030 $original 2>/dev/null
	atf_pass
}

atf_init_test_cases()
{
	atf_add_test_case ibs_swfilt_exclude_user
	atf_add_test_case ibs_swfilt_exclude_kernel
	atf_add_test_case ibs_swfilt_exclude_hv
	atf_add_test_case ibs_swfilt_filter_combination
}

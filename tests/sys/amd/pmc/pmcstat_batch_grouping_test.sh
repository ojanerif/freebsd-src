#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Validate pmcstat(8) -b brace-list grouping on AMD Zen core PMCs without
#   exercising multiplexing.  Every event set below fits within the baseline
#   six-counter AMD Zen core PMC pool.

pmcstat_batch_check_support()
{
	if ! kldstat -n hwpmc > /dev/null 2>&1 &&
	    ! sysctl -n kern.hwpmc.cpuid > /dev/null 2>&1; then
		atf_skip "hwpmc unavailable: no hwpmc KLD and no kern.hwpmc.cpuid"
	fi
	if ! command -v pmcstat > /dev/null 2>&1; then
		atf_skip "pmcstat not found in PATH"
	fi
	if ! command -v dd > /dev/null 2>&1; then
		atf_skip "dd not found in PATH"
	fi
	if ! pmcstat -b -L > /dev/null 2>pmcstat-b-support.err; then
		if grep -qi 'illegal option' pmcstat-b-support.err; then
			atf_skip "pmcstat -b is not available on this userland"
		fi
		atf_skip "pmcstat -b -L failed; see pmcstat-b-support.err"
	fi
}

pmcstat_batch_check_runtime_enabled()
{
	if [ "$(atf_config_get amd.pmc.grouping.runtime false)" != "true" ]; then
		atf_skip "AMD core grouping runtime disabled by default; set amd.pmc.grouping.runtime=true"
	fi
}

pmcstat_batch_check_known_zen()
{
	local cpuid family model oldifs stepping vendor zen

	cpuid=$(sysctl -n kern.hwpmc.cpuid 2>/dev/null) || \
	    atf_skip "kern.hwpmc.cpuid is unavailable"
	oldifs=$IFS
	IFS=-
	set -- $cpuid
	IFS=$oldifs
	vendor=$1
	family=$2
	model=$3
	stepping=$4
	if [ "$vendor" != "AuthenticAMD" ]; then
		atf_skip "AMD core grouping runtime requires AuthenticAMD CPU, got $cpuid"
	fi
	case "$family" in
	*[!0-9]*|'')
		atf_skip "cannot parse AMD family from kern.hwpmc.cpuid=$cpuid"
		;;
	esac
	case "$model" in
	[0-9A-F][0-9A-F]) ;;
	*) atf_skip "cannot parse AMD model from kern.hwpmc.cpuid=$cpuid" ;;
	esac
	case "$stepping" in
	[0-9A-F]) ;;
	*) atf_skip "cannot parse AMD stepping from kern.hwpmc.cpuid=$cpuid" ;;
	esac

	zen=
	case "$family" in
	23)
		case "$model" in
		0[1-7]|1[1-7]) zen="Zen 1" ;;
		08|09|0A|0B|0C|0D|0E|0F|1[8-9A-F]) zen="Zen+" ;;
		3[1-9A-F]|6[0-9A-F]|7[0-9A-F]|9[0-9A-F]|A[0-9A-F]) zen="Zen 2" ;;
		esac
		;;
	25)
		case "$model" in
		0[0-9A-F]|2[0-9A-F]|5[0-9A-F]) zen="Zen 3" ;;
		4[0-9A-F]) zen="Zen 3+" ;;
		1[0-9A-F]|6[0-9A-F]|7[0-9A-F]|A[0-9A-F]) zen="Zen 4" ;;
		esac
		;;
	26)
		case "$model" in
		0[0-9A-F]|1[0-9A-F]|2[0-9A-F]|3[0-9A-F]|4[0-9A-F]|6[0-9A-F]|7[0-9A-F])
			zen="Zen 5" ;;
		5[0-9A-F]|8[0-9A-F]|9[0-9A-F]|A[0-9A-F]|C[0-9A-F])
			zen="Zen 6" ;;
		esac
		;;
	esac
	if [ -z "$zen" ]; then
		atf_skip "AMD Family ${family} Model ${model} is not in the validated Zen map"
	fi
	printf 'AMD core PMC grouping target: %s family=%s model=%s stepping=%s\n' \
	    "$zen" "$family" "$model" "$stepping"
}

pmcstat_batch_check_runtime()
{
	pmcstat_batch_check_support
	pmcstat_batch_check_runtime_enabled
	pmcstat_batch_check_known_zen
}

pmcstat_batch_event_available()
{
	local event="$1"

	pmcstat -C -q -p "$event" -o /dev/null -- /usr/bin/true \
	    > /dev/null 2>pmcstat-event.err
}

pmcstat_batch_require_event()
{
	local event="$1"

	if ! pmcstat_batch_event_available "$event"; then
		atf_skip "pmcstat event $event is unavailable on this CPU"
	fi
}

pmcstat_batch_require_events()
{
	local event

	for event in "$@"; do
		pmcstat_batch_require_event "$event"
	done
}

pmcstat_batch_capture()
{
	local block_count err out

	out="$1"
	shift
	block_count=$(atf_config_get amd.pmc.grouping.batch_blocks 250000)
	case "$block_count" in
	*[!0-9]*|'')
		atf_fail "invalid amd.pmc.grouping.batch_blocks=$block_count"
		;;
	esac
	if [ "$block_count" -le 0 ]; then
		atf_fail "amd.pmc.grouping.batch_blocks must be positive"
	fi
	if [ "$block_count" -gt 5000000 ]; then
		atf_fail "amd.pmc.grouping.batch_blocks=$block_count is too large; maximum is 5000000"
	fi
	if command -v cpuset > /dev/null 2>&1; then
		cpuset -l 0 pmcstat -b -C -q "$@" -o "$out" -- \
		    dd if=/dev/zero of=/dev/null bs=4096 count="$block_count" \
		    > /dev/null 2>pmcstat.err
	else
		pmcstat -b -C -q "$@" -o "$out" -- \
		    dd if=/dev/zero of=/dev/null bs=4096 count="$block_count" \
		    > /dev/null 2>pmcstat.err
	fi
	if [ "$?" -ne 0 ]; then
		err=$(cat pmcstat.err)
		atf_fail "pmcstat -b failed: $err"
	fi
	if [ ! -s "$out" ]; then
		atf_fail "pmcstat -b produced an empty output file"
	fi
}

pmcstat_batch_header_event_count()
{
	local count event expected header

	event="$1"
	expected="$2"
	header="$3"
	count=$(printf '%s\n' "$header" | awk -v event="$event" '
	    {
	        for (i = 1; i <= NF; i++) {
	            if ($i == event)
	                n++;
	            else if (length($i) > length(event) &&
	                substr($i, length($i) - length(event) + 1) == event &&
	                substr($i, length($i) - length(event), 1) == "/")
	                n++;
	        }
	    }
	    END { print n + 0 }')
	if [ "$count" -ne "$expected" ]; then
		atf_fail "header has $count copies of $event, expected $expected: $header"
	fi
}

pmcstat_batch_numeric_rows()
{
	local file="$1"
	local columns="$2"

	awk -v columns="$columns" '
	    !/^#/ {
	        ok = 1;
	        for (i = 1; i <= columns; i++)
	            if ($i !~ /^[0-9]+$/)
	                ok = 0;
	        if (ok)
	            rows++;
	    }
	    END { print rows + 0 }' "$file"
}

pmcstat_batch_positive_columns()
{
	local file="$1"
	local columns="$2"

	awk -v columns="$columns" '
	    !/^#/ {
	        ok = 1;
	        for (i = 1; i <= columns; i++)
	            if ($i !~ /^[0-9]+$/)
	                ok = 0;
	        if (ok) {
	            for (i = 1; i <= columns; i++)
	                if ($i > 0)
	                    pos[i] = 1;
	        }
	    }
	    END {
	        for (i = 1; i <= columns; i++)
	            if (!pos[i])
	                missing++;
	        print missing + 0;
	    }' "$file"
}

atf_test_case brace_process_group_three_events_counts cleanup
brace_process_group_three_events_counts_head()
{
	atf_set "descr" "pmcstat -b process brace group with three fitting AMD core events counts all columns"
	atf_set "require.user" "root"
}
brace_process_group_three_events_counts_body()
{
	local header missing rows

	pmcstat_batch_check_runtime
	pmcstat_batch_require_events instructions unhalted-cycles branches
	pmcstat_batch_capture pmcstat-b-three.out \
	    -p '{instructions,unhalted-cycles,branches}'
	header=$(grep '^#' pmcstat-b-three.out | grep 'instructions' |
	    grep 'unhalted-cycles' | grep 'branches' | head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat -b output is missing the three-event group header"
	fi
	pmcstat_batch_header_event_count instructions 1 "$header"
	pmcstat_batch_header_event_count unhalted-cycles 1 "$header"
	pmcstat_batch_header_event_count branches 1 "$header"
	rows=$(pmcstat_batch_numeric_rows pmcstat-b-three.out 3)
	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat -b produced no parseable three-counter rows"
	fi
	missing=$(pmcstat_batch_positive_columns pmcstat-b-three.out 3)
	if [ "$missing" -ne 0 ]; then
		atf_fail "pmcstat -b three-event group left $missing columns at zero"
	fi
}
brace_process_group_three_events_counts_cleanup()
{
	rm -f pmcstat-b-three.out pmcstat.err pmcstat-b-support.err
	rm -f pmcstat-event.err
}

atf_test_case two_brace_process_groups_fit_without_mux cleanup
two_brace_process_groups_fit_without_mux_head()
{
	atf_set "descr" "two pmcstat -b process groups whose combined AMD core events fit without multiplexing"
	atf_set "require.user" "root"
}
two_brace_process_groups_fit_without_mux_body()
{
	local header rows

	#
	# Use the generic branch-mispredicts alias rather than the Linux-style
	# branch-misses name: libpmc maps branch-mispredicts to ex_ret_brn_misp
	# on AMD PMU tables.  The other three events here are likewise generic
	# aliases, so the group stays on portable AMD core events.
	#
	pmcstat_batch_check_runtime
	pmcstat_batch_require_events instructions unhalted-cycles branches \
	    branch-mispredicts
	pmcstat_batch_capture pmcstat-b-two-groups.out \
	    -p '{instructions,unhalted-cycles}' \
	    -p '{branches,branch-mispredicts}'
	header=$(grep '^#' pmcstat-b-two-groups.out | grep 'instructions' |
	    grep 'unhalted-cycles' | grep 'branches' | grep 'branch-mispredicts' |
	    head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat -b output is missing the two-group header"
	fi
	pmcstat_batch_header_event_count instructions 1 "$header"
	pmcstat_batch_header_event_count unhalted-cycles 1 "$header"
	pmcstat_batch_header_event_count branches 1 "$header"
	pmcstat_batch_header_event_count branch-mispredicts 1 "$header"
	rows=$(pmcstat_batch_numeric_rows pmcstat-b-two-groups.out 4)
	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat -b produced no parseable four-counter rows"
	fi
}
two_brace_process_groups_fit_without_mux_cleanup()
{
	rm -f pmcstat-b-two-groups.out pmcstat.err pmcstat-b-support.err
	rm -f pmcstat-event.err
}

atf_test_case mixed_brace_and_plain_process_events cleanup
mixed_brace_and_plain_process_events_head()
{
	atf_set "descr" "pmcstat -b accepts one fitting brace group plus one ungrouped process event"
	atf_set "require.user" "root"
}
mixed_brace_and_plain_process_events_body()
{
	local header rows

	pmcstat_batch_check_runtime
	pmcstat_batch_require_events instructions unhalted-cycles branches
	pmcstat_batch_capture pmcstat-b-mixed.out \
	    -p '{instructions,unhalted-cycles}' \
	    -p branches
	header=$(grep '^#' pmcstat-b-mixed.out | grep 'instructions' |
	    grep 'unhalted-cycles' | grep 'branches' | head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat -b output is missing the mixed grouped/plain header"
	fi
	pmcstat_batch_header_event_count instructions 1 "$header"
	pmcstat_batch_header_event_count unhalted-cycles 1 "$header"
	pmcstat_batch_header_event_count branches 1 "$header"
	rows=$(pmcstat_batch_numeric_rows pmcstat-b-mixed.out 3)
	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat -b mixed run produced no parseable rows"
	fi
}
mixed_brace_and_plain_process_events_cleanup()
{
	rm -f pmcstat-b-mixed.out pmcstat.err pmcstat-b-support.err
	rm -f pmcstat-event.err
}

atf_test_case malformed_brace_group_rejected cleanup
malformed_brace_group_rejected_head()
{
	atf_set "descr" "pmcstat -b rejects malformed brace-list event groups without crashing"
	atf_set "require.user" "root"
}
malformed_brace_group_rejected_body()
{
	local rc

	pmcstat_batch_check_runtime
	pmcstat_batch_require_events instructions unhalted-cycles
	pmcstat -b -C -q -p '{instructions,unhalted-cycles' \
	    -o pmcstat-b-malformed.out -- /usr/bin/true \
	    > /dev/null 2>pmcstat.err
	rc=$?
	if [ "$rc" -eq 0 ]; then
		atf_fail "pmcstat -b accepted a malformed brace group"
	fi
	if [ "$rc" -ge 128 ]; then
		atf_fail "pmcstat -b malformed group died by signal exit=$rc"
	fi
	if [ ! -s pmcstat.err ]; then
		atf_fail "pmcstat -b malformed group failed without diagnostics"
	fi
}
malformed_brace_group_rejected_cleanup()
{
	rm -f pmcstat-b-malformed.out pmcstat.err pmcstat-b-support.err
	rm -f pmcstat-event.err
}

atf_init_test_cases()
{
	atf_add_test_case brace_process_group_three_events_counts
	atf_add_test_case two_brace_process_groups_fit_without_mux
	atf_add_test_case mixed_brace_and_plain_process_events
	atf_add_test_case malformed_brace_group_rejected
}

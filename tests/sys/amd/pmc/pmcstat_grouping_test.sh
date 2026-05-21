#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Verify pmcstat(8) repeated-event counting behavior.  The default cases use
#   software PMCs as a safe baseline; opt-in AMD core PMU cases cover realistic
#   duplicate-counter skew, mixed events, and oversubscription behavior.

pmcstat_check_support()
{
	if ! kldstat -n hwpmc > /dev/null 2>&1; then
		atf_skip "hwpmc module not loaded (kldload hwpmc)"
	fi
	if ! command -v pmcstat > /dev/null 2>&1; then
		atf_skip "pmcstat not found in PATH"
	fi
}

pmcstat_check_grouping_runtime_enabled()
{
	if [ "$(atf_config_get amd.pmc.grouping.runtime false)" != "true" ]; then
		atf_skip "AMD core grouping runtime disabled by default; set amd.pmc.grouping.runtime=true"
	fi
}

pmcstat_check_amd_cpuid()
{
	local cpuid

	cpuid=$(sysctl -n kern.hwpmc.cpuid 2>/dev/null) || \
	    atf_skip "kern.hwpmc.cpuid is unavailable"
	case "$cpuid" in
	AuthenticAMD-*)
		;;
	*)
		atf_skip "AMD core grouping runtime requires AuthenticAMD CPU, got $cpuid"
		;;
	esac
}

pmcstat_check_amd_grouping_runtime()
{
	pmcstat_check_grouping_runtime_enabled
	pmcstat_check_amd_cpuid
}

pmcstat_cycle_tolerance_permille()
{
	local tolerance

	tolerance=$(atf_config_get amd.pmc.grouping.cycle_tolerance_permille 250)
	case "$tolerance" in
	*[!0-9]*|'')
		atf_fail "invalid amd.pmc.grouping.cycle_tolerance_permille=$tolerance"
		;;
	esac
	printf '%s\n' "$tolerance"
}

pmcstat_event_available()
{
	local event="$1"

	pmcstat -L | awk -v event="$event" '
	    $1 == event { found = 1 }
	    END { exit(found ? 0 : 1) }'
}

pmcstat_require_event()
{
	local event="$1"

	if ! pmcstat_event_available "$event"; then
		atf_skip "pmcstat event $event is unavailable on this CPU"
	fi
}

pmcstat_first_available_event()
{
	local event

	for event in "$@"; do
		if pmcstat_event_available "$event"; then
			printf '%s\n' "$event"
			return 0
		fi
	done
	return 1
}

pmcstat_capture_two_events()
{
	local duration
	local err
	local event_a="$2"
	local event_b="$3"
	local out="$1"

	duration="${4:-2}"
	if ! pmcstat -c 0 -s "$event_a" -s "$event_b" -w 1 \
	    -o "$out" sleep "$duration" > /dev/null 2>pmcstat.err; then
		err=$(cat pmcstat.err)
		atf_fail "pmcstat failed with $event_a,$event_b: $err"
	fi
	if [ ! -s "$out" ]; then
		atf_fail "pmcstat produced an empty counting output file"
	fi
}

pmcstat_capture_process_two_events()
{
	local duration
	local err
	local event_a="$2"
	local event_b="$3"
	local out="$1"

	duration="${4:-2}"
	if ! pmcstat -C -q -p "$event_a" -p "$event_b" -o "$out" -- \
	    sleep "$duration" > /dev/null 2>pmcstat.err; then
		err=$(cat pmcstat.err)
		atf_fail "pmcstat failed with $event_a,$event_b: $err"
	fi
	if [ ! -s "$out" ]; then
		atf_fail "pmcstat produced an empty process-counting output file"
	fi
}

pmcstat_require_header_event_count()
{
	local count
	local event="$1"
	local expected="$2"
	local header="$3"

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

pmcstat_two_counter_stats()
{
	local file="$1"

	awk '
	    !/^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
	        rows++;
	        if ($1 > 0)
	            positive_a++;
	        if ($2 > 0)
	            positive_b++;
	        last_a = $1;
	        last_b = $2;
	    }
	    END { print rows + 0, positive_a + 0, positive_b + 0,
	        last_a + 0, last_b + 0 }' "$file"
}

pmcstat_require_bounded_delta()
{
	local a="$1"
	local b="$2"
	local limit_permille="$3"
	local context="$4"
	local delta_report

	delta_report=$(awk -v a="$a" -v b="$b" '
	    BEGIN {
	        d = a - b;
	        if (d < 0)
	            d = -d;
	        m = (a > b) ? a : b;
	        if (m <= 0) {
	            print "invalid";
	            exit 2;
	        }
	        printf "delta=%0.f max=%0.f permille=%.3f", d, m, (d * 1000) / m;
	    }')
	if ! awk -v a="$a" -v b="$b" -v limit="$limit_permille" '
	    BEGIN {
	        d = a - b;
	        if (d < 0)
	            d = -d;
	        m = (a > b) ? a : b;
	        exit(!(m > 0 && d * 1000 <= limit * m));
	    }'; then
		atf_fail "$context counters diverged beyond ${limit_permille}/1000: $delta_report"
	fi
	printf '%s observed bounded skew: %s\n' "$context" "$delta_report"
}

atf_test_case multiple_system_events_are_independent_columns cleanup
multiple_system_events_are_independent_columns_head()
{
	atf_set "descr" "pmcstat repeated -s options produce two counter columns"
	atf_set "require.user" "root"
}
multiple_system_events_are_independent_columns_body()
{
	local header rows

	pmcstat_check_support
	pmcstat_capture_two_events "pmcstat.out" \
	    "SOFT-CLOCK.HARD" "SOFT-CLOCK.STAT"

	header=$(grep '^#' pmcstat.out | grep 'SOFT-CLOCK.HARD' |
	    grep 'SOFT-CLOCK.STAT' | head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat output is missing the two-counter header"
	fi
	if ! echo "$header" | grep -q 'SOFT-CLOCK.HARD'; then
		atf_fail "header missing SOFT-CLOCK.HARD column: $header"
	fi
	if ! echo "$header" | grep -q 'SOFT-CLOCK.STAT'; then
		atf_fail "header missing SOFT-CLOCK.STAT column: $header"
	fi

	rows=$(awk '!/^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ { n++ }
	    END { print n + 0 }' pmcstat.out)
	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat produced no rows with two numeric counters"
	fi
}
multiple_system_events_are_independent_columns_cleanup()
{
	rm -f pmcstat.out pmcstat.err
}

atf_test_case same_system_event_group_has_same_count cleanup
same_system_event_group_has_same_count_head()
{
	atf_set "descr" "pmcstat grouped identical events produce matching counts"
	atf_set "require.user" "root"
}
same_system_event_group_has_same_count_body()
{
	local checked header mismatches positive rows stats

	pmcstat_check_support
	pmcstat_capture_two_events "pmcstat-same.out" \
	    "SOFT-CLOCK.HARD" "SOFT-CLOCK.HARD" 4

	header=$(grep '^#' pmcstat-same.out | grep 'SOFT-CLOCK.HARD' |
	    head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat output is missing the grouped-event header"
	fi
	pmcstat_require_header_event_count "SOFT-CLOCK.HARD" 2 "$header"

	stats=$(awk '
	    !/^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
	        rows++;
	        if (rows == 1)
	            next;
	        checked++;
	        if ($1 != $2) {
	            mismatches++;
	            bad_row = rows;
	            bad_a = $1;
	            bad_b = $2;
	        }
	        if ($1 > 0 && $2 > 0)
	            positive++;
	    }
	    END { print rows + 0, checked + 0, mismatches + 0, positive + 0,
	        bad_row + 0, bad_a + 0, bad_b + 0 }' pmcstat-same.out)
	set -- $stats
	rows=$1
	checked=$2
	mismatches=$3
	positive=$4

	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat produced no numeric rows for grouped identical events"
	fi
	if [ "$checked" -eq 0 ]; then
		atf_fail "pmcstat produced no post-warmup rows for grouped identical events"
	fi
	if [ "$positive" -eq 0 ]; then
		atf_fail "grouped identical events never produced a positive count"
	fi
	if [ "$mismatches" -ne 0 ]; then
		atf_fail "grouped identical events mismatched: count=$mismatches row=$5"
	fi
}
same_system_event_group_has_same_count_cleanup()
{
	rm -f pmcstat-same.out pmcstat.err
}

atf_test_case repeated_process_cycles_have_bounded_skew cleanup
repeated_process_cycles_have_bounded_skew_head()
{
	atf_set "descr" "duplicate AMD core cycle PMCs may differ only within a bounded tolerance"
	atf_set "require.user" "root"
}
repeated_process_cycles_have_bounded_skew_body()
{
	local cycles_a cycles_b header positive_a positive_b rows stats tolerance

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	pmcstat_capture_process_two_events "pmcstat-cycles-skew.out" \
	    "ls_not_halted_cyc" "ls_not_halted_cyc" 5
	header=$(grep '^#' pmcstat-cycles-skew.out | grep 'ls_not_halted_cyc' |
	    head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat output is missing the duplicate-cycle header"
	fi
	pmcstat_require_header_event_count "ls_not_halted_cyc" 2 "$header"

	stats=$(pmcstat_two_counter_stats pmcstat-cycles-skew.out)
	set -- $stats
	rows=$1
	positive_a=$2
	positive_b=$3
	cycles_a=$4
	cycles_b=$5

	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat produced no numeric rows for duplicate cycle PMCs"
	fi
	if [ "$positive_a" -eq 0 ] || [ "$positive_b" -eq 0 ]; then
		atf_fail "duplicate cycle PMCs did not both produce positive counts"
	fi
	tolerance=$(pmcstat_cycle_tolerance_permille)
	pmcstat_require_bounded_delta "$cycles_a" "$cycles_b" "$tolerance" \
	    "duplicate ls_not_halted_cyc"
}
repeated_process_cycles_have_bounded_skew_cleanup()
{
	rm -f pmcstat-cycles-skew.out pmcstat.err
}

atf_test_case mixed_cycles_instructions_are_independent_columns cleanup
mixed_cycles_instructions_are_independent_columns_head()
{
	atf_set "descr" "cycles and instructions remain distinct pmcstat process-counting columns"
	atf_set "require.user" "root"
}
mixed_cycles_instructions_are_independent_columns_body()
{
	local header positive_a positive_b rows stats

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"
	pmcstat_require_event "ex_ret_instr"

	pmcstat_capture_process_two_events "pmcstat-mixed-ipc.out" \
	    "ls_not_halted_cyc" "ex_ret_instr" 3
	header=$(grep '^#' pmcstat-mixed-ipc.out | grep 'ls_not_halted_cyc' |
	    grep 'ex_ret_instr' | head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat output is missing cycles+instructions header"
	fi
	pmcstat_require_header_event_count "ls_not_halted_cyc" 1 "$header"
	pmcstat_require_header_event_count "ex_ret_instr" 1 "$header"
	stats=$(pmcstat_two_counter_stats pmcstat-mixed-ipc.out)
	set -- $stats
	rows=$1
	positive_a=$2
	positive_b=$3
	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat produced no numeric rows for cycles+instructions"
	fi
	if [ "$positive_a" -eq 0 ] || [ "$positive_b" -eq 0 ]; then
		atf_fail "cycles+instructions did not both produce positive counts"
	fi
}
mixed_cycles_instructions_are_independent_columns_cleanup()
{
	rm -f pmcstat-mixed-ipc.out pmcstat.err
}

atf_test_case mixed_cache_cycles_are_independent_columns cleanup
mixed_cache_cycles_are_independent_columns_head()
{
	atf_set "descr" "cache-related miss and cycles remain distinct pmcstat process-counting columns"
	atf_set "require.user" "root"
}
mixed_cache_cycles_are_independent_columns_body()
{
	local cache_event header positive_b rows stats

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	cache_event=$(pmcstat_first_available_event \
	    "l2_pf_miss_l2_l3.all" "l2_pf_miss_l2_l3" \
	    "l2_pf_miss_l2_l3.l2_hwpf") || \
	    atf_skip "no supported AMD L2 prefetch miss event is available"
	pmcstat_capture_process_two_events "pmcstat-mixed-cache.out" \
	    "$cache_event" "ls_not_halted_cyc" 3
	header=$(grep '^#' pmcstat-mixed-cache.out | grep "$cache_event" |
	    grep 'ls_not_halted_cyc' | head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat output is missing cache+cycles header"
	fi
	pmcstat_require_header_event_count "$cache_event" 1 "$header"
	pmcstat_require_header_event_count "ls_not_halted_cyc" 1 "$header"
	stats=$(pmcstat_two_counter_stats pmcstat-mixed-cache.out)
	set -- $stats
	rows=$1
	positive_b=$3
	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat produced no numeric rows for cache+cycles"
	fi
	if [ "$positive_b" -eq 0 ]; then
		atf_fail "cache+cycles run did not produce positive cycle counts"
	fi
}
mixed_cache_cycles_are_independent_columns_cleanup()
{
	rm -f pmcstat-mixed-cache.out pmcstat.err
}

atf_test_case oversubscribed_process_cycles_fail_cleanly cleanup
oversubscribed_process_cycles_fail_cleanly_head()
{
	atf_set "descr" "oversubscribed duplicate AMD cycle PMCs fail cleanly or produce parseable output"
	atf_set "require.user" "root"
}
oversubscribed_process_cycles_fail_cleanly_body()
{
	local err rows

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	if pmcstat -C -q \
	    -p ls_not_halted_cyc -p ls_not_halted_cyc \
	    -p ls_not_halted_cyc -p ls_not_halted_cyc \
	    -p ls_not_halted_cyc -p ls_not_halted_cyc \
	    -p ls_not_halted_cyc -o pmcstat-oversub.out -- \
	    sleep 1 > /dev/null 2>pmcstat.err; then
		if [ ! -s pmcstat-oversub.out ]; then
			atf_fail "oversubscribed pmcstat run succeeded with empty output"
		fi
		rows=$(awk '
		    !/^#/ && NF >= 7 {
		        ok = 1;
		        for (i = 1; i <= 7; i++)
		            if ($i !~ /^[0-9]+$/)
		                ok = 0;
		        if (ok)
		            n++;
		    }
		    END { print n + 0 }' pmcstat-oversub.out)
		if [ "$rows" -eq 0 ]; then
			atf_fail "oversubscribed pmcstat run has no parseable 7-counter row"
		fi
		printf 'oversubscribed duplicate cycles succeeded with parseable output\n'
	else
		if [ ! -s pmcstat.err ]; then
			atf_fail "oversubscribed pmcstat run failed without diagnostics"
		fi
		err=$(cat pmcstat.err)
		printf 'oversubscribed duplicate cycles failed cleanly: %s\n' "$err"
	fi
}
oversubscribed_process_cycles_fail_cleanly_cleanup()
{
	rm -f pmcstat-oversub.out pmcstat.err
}

atf_init_test_cases()
{
	atf_add_test_case multiple_system_events_are_independent_columns
	atf_add_test_case same_system_event_group_has_same_count
	atf_add_test_case repeated_process_cycles_have_bounded_skew
	atf_add_test_case mixed_cycles_instructions_are_independent_columns
	atf_add_test_case mixed_cache_cycles_are_independent_columns
	atf_add_test_case oversubscribed_process_cycles_fail_cleanly
}

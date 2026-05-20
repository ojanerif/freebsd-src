#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Verify pmcstat(8) grouped system-counting output with software PMCs.
#   The tests check that repeated -s options remain independent columns and
#   that grouping the same event twice reports matching counts.

pmcstat_check_support()
{
	if ! kldstat -n hwpmc > /dev/null 2>&1; then
		atf_skip "hwpmc module not loaded (kldload hwpmc)"
	fi
	if ! command -v pmcstat > /dev/null 2>&1; then
		atf_skip "pmcstat not found in PATH"
	fi
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

pmcstat_require_header_event_count()
{
	local count
	local event="$1"
	local expected="$2"
	local header="$3"

	count=$(printf '%s\n' "$header" | awk -v event="$event" \
	    '{ for (i = 1; i <= NF; i++) if ($i == event) n++ }
	    END { print n + 0 }')
	if [ "$count" -ne "$expected" ]; then
		atf_fail "header has $count copies of $event, expected $expected: $header"
	fi
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

atf_init_test_cases()
{
	atf_add_test_case multiple_system_events_are_independent_columns
	atf_add_test_case same_system_event_group_has_same_count
}

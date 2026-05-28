#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Shell characterization for concurrent pmcstat(8) process-scope AMD core
#   PMC allocation attempts.

pmc_atomicity_check_support()
{
	if ! kldstat -n hwpmc > /dev/null 2>&1; then
		atf_skip "hwpmc module not loaded (kldload hwpmc)"
	fi
	if ! command -v pmcstat > /dev/null 2>&1; then
		atf_skip "pmcstat not found in PATH"
	fi
	if ! command -v timeout > /dev/null 2>&1; then
		atf_skip "timeout not found in PATH; required for bounded FIFO release"
	fi
}

pmc_atomicity_check_runtime_enabled()
{
	if [ "$(atf_config_get amd.pmc.grouping.runtime false)" != "true" ]; then
		atf_skip "AMD core grouping runtime disabled by default; set amd.pmc.grouping.runtime=true"
	fi
}

pmc_atomicity_check_amd_cpuid()
{
	local cpuid

	cpuid=$(sysctl -n kern.hwpmc.cpuid 2>/dev/null) || \
	    atf_skip "kern.hwpmc.cpuid is unavailable"
	case "$cpuid" in
	AuthenticAMD-*) ;;
	*) atf_skip "AMD core PMC runtime requires AuthenticAMD CPU, got $cpuid" ;;
	esac
}

pmc_atomicity_check_known_zen()
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
		atf_skip "AMD core PMC runtime requires AuthenticAMD CPU, got $cpuid"
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
		3[1-9A-F]|6[0-9A-F]|7[1-9A-F]|9[0-9A-F]|A[0-9A-F]) zen="Zen 2" ;;
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
		0[0-9A-F]|1[0-9A-F]|2[0-9A-F]|4[0-9A-F]|6[0-9A-F]|7[0-9A-F]) zen="Zen 5" ;;
		5[0-9A-F]|8[0-9A-F]|9[0-9A-F]|A[0-9A-F]|C[0-9A-F]) zen="Zen 6" ;;
		esac
		;;
	esac
	if [ -z "$zen" ]; then
		atf_skip "AMD Family ${family} Model ${model} is not in the validated Zen map"
	fi
	printf 'AMD core PMC runtime target: %s family=%s model=%s stepping=%s\n' \
	    "$zen" "$family" "$model" "$stepping"
}

pmc_atomicity_event_available()
{
	local event="$1"

	pmcstat -L | awk -v event="$event" '
	    $1 == event { found = 1 }
	    END { exit(found ? 0 : 1) }'
}

pmc_atomicity_require_event()
{
	local event="$1"

	if ! pmc_atomicity_event_available "$event"; then
		atf_skip "pmcstat event $event is unavailable on this CPU"
	fi
}

pmc_atomicity_helper()
{
	printf '%s\n' "$(atf_get_srcdir)/pmcinfo_thread_count"
}

pmc_atomicity_require_helper()
{
	atf_require_prog "$(pmc_atomicity_helper)"
}

pmc_atomicity_read_k8_thread_rows()
{
	local count err helper lines out

	out="$1"
	err="$2"
	helper=$(pmc_atomicity_helper)
	if ! "$helper" k8-thread > "$out" 2> "$err"; then
		return 1
	fi
	lines=$(awk 'END { print NR + 0 }' "$out") || return 1
	if [ "$lines" -ne 1 ]; then
		printf 'expected one pmcinfo_thread_count stdout line, saw %s\n' \
		    "$lines" >> "$err"
		return 1
	fi
	if ! read count < "$out"; then
		return 1
	fi
	case "$count" in
	*[!0-9]*|'') return 1 ;;
	esac
	printf '%s\n' "$count"
}

pmc_atomicity_assert_k8_thread_baseline()
{
	local baseline count err out

	baseline="$1"
	out="$2"
	err="$3"
	count=$(pmc_atomicity_read_k8_thread_rows "$out" "$err") || \
	    atf_fail "pmcinfo_thread_count k8-thread failed; see $err"
	if [ "$count" -ne "$baseline" ]; then
		atf_fail "AMD PMC THREAD row count changed from baseline $baseline to $count; see $out"
	fi
}

pmc_atomicity_iterations()
{
	local iterations

	iterations=$(atf_config_get amd.pmc.grouping.atomicity.iterations 50)
	case "$iterations" in
	*[!0-9]*|'')
		printf 'invalid amd.pmc.grouping.atomicity.iterations=%s\n' \
		    "$iterations" >&2
		return 1
		;;
	esac
	if [ "$iterations" -le 0 ]; then
		printf 'amd.pmc.grouping.atomicity.iterations must be positive\n' >&2
		return 1
	fi
	if [ "$iterations" -gt 500 ]; then
		printf 'amd.pmc.grouping.atomicity.iterations=%s is too large; maximum is 500\n' \
		    "$iterations" >&2
		return 1
	fi
	printf '%s\n' "$iterations"
}

pmc_atomicity_run_pmcstat()
{
	local event="$1"
	local start_fifo="$2"
	local out="$3"
	local err="$4"

	read _ < "$start_fifo"
	exec pmcstat -C -q \
	    -p "$event" -p "$event" -p "$event" \
	    -p "$event" -p "$event" -p "$event" \
	    -o "$out" -- sleep 1 > /dev/null 2> "$err"
}

pmc_atomicity_release_fifo()
{
	local fifo="$1"

	timeout 30 sh -c 'printf "go\n" > "$1"' sh "$fifo"
}

pmc_atomicity_cleanup_lock()
{
	local owner

	if [ -f pmc_atomicity.lock.owner ]; then
		if read owner < pmc_atomicity.lock.owner; then
			rm -f "$owner"
		fi
		rm -f pmc_atomicity.lock.owner
	fi
	rmdir /var/run/pmc_atomicity.lock.d 2>/dev/null || true
}

atf_test_case concurrent_process_allocations_no_residue cleanup
concurrent_process_allocations_no_residue_head()
{
	atf_set "descr" "Race two full-width AMD pmcstat process allocations and check for residue"
	atf_set "require.user" "root"
	atf_set "is_exclusive" "true"
}
concurrent_process_allocations_no_residue_body()
{
	local baseline err_a err_b event fail_msg fifo_a fifo_b i iterations lockdir
	local out_a out_b pid_a pid_b rc_a rc_b successes

	pmc_atomicity_check_support
	pmc_atomicity_check_runtime_enabled
	pmc_atomicity_check_amd_cpuid
	pmc_atomicity_check_known_zen
	event="ls_not_halted_cyc"
	pmc_atomicity_require_event "$event"
	pmc_atomicity_require_helper
	iterations=$(pmc_atomicity_iterations) || \
	    atf_fail "invalid amd.pmc.grouping.atomicity.iterations"
	lockdir="/var/run/pmc_atomicity.lock.d"
	trap pmc_atomicity_cleanup_lock EXIT
	trap 'pmc_atomicity_cleanup_lock; exit 1' HUP INT TERM
	if ! mkdir "$lockdir" 2>/dev/null; then
		atf_skip "another PMC atomicity test owns $lockdir"
	fi
	if ! printf '%s\n' "$$" > "$lockdir/owner"; then
		rmdir "$lockdir" 2>/dev/null || true
		atf_fail "failed to write $lockdir/owner"
	fi
	printf '%s\n' "$lockdir/owner" > pmc_atomicity.lock.owner
	baseline=$(pmc_atomicity_read_k8_thread_rows \
	    pmcinfo-baseline.out pmcinfo-baseline.err) || \
	    atf_fail "pmcinfo_thread_count k8-thread failed before race; see pmcinfo-baseline.err"
	successes=0

	for i in $(jot "$iterations"); do
		fifo_a="start-a.$i.fifo"
		fifo_b="start-b.$i.fifo"
		out_a="pmcstat-a.$i.out"
		out_b="pmcstat-b.$i.out"
		err_a="pmcstat-a.$i.err"
		err_b="pmcstat-b.$i.err"
		mkfifo "$fifo_a" "$fifo_b" || atf_fail "mkfifo iteration $i failed"
		pmc_atomicity_run_pmcstat "$event" "$fifo_a" "$out_a" "$err_a" &
		pid_a=$!
		pmc_atomicity_run_pmcstat "$event" "$fifo_b" "$out_b" "$err_b" &
		pid_b=$!
		if ! pmc_atomicity_release_fifo "$fifo_a"; then
			kill "$pid_a" "$pid_b" 2>/dev/null || true
			wait "$pid_a" 2>/dev/null || true
			wait "$pid_b" 2>/dev/null || true
			atf_fail "iteration $i failed to release first pmcstat child"
		fi
		if ! pmc_atomicity_release_fifo "$fifo_b"; then
			kill "$pid_a" "$pid_b" 2>/dev/null || true
			wait "$pid_a" 2>/dev/null || true
			wait "$pid_b" 2>/dev/null || true
			atf_fail "iteration $i failed to release second pmcstat child"
		fi
		wait "$pid_a"; rc_a=$?
		wait "$pid_b"; rc_b=$?
		if [ "$rc_a" -ne 0 ] && [ ! -s "$err_a" ]; then
			atf_fail "iteration $i first pmcstat failed without diagnostics"
		fi
		if [ "$rc_b" -ne 0 ] && [ ! -s "$err_b" ]; then
			atf_fail "iteration $i second pmcstat failed without diagnostics"
		fi
		if [ "$rc_a" -eq 0 ] && [ ! -s "$out_a" ]; then
			atf_fail "iteration $i first pmcstat succeeded with empty output"
		fi
		if [ "$rc_b" -eq 0 ] && [ ! -s "$out_b" ]; then
			atf_fail "iteration $i second pmcstat succeeded with empty output"
		fi
		if [ "$rc_a" -eq 0 ]; then
			successes=$((successes + 1))
		fi
		if [ "$rc_b" -eq 0 ]; then
			successes=$((successes + 1))
		fi
		pmc_atomicity_assert_k8_thread_baseline "$baseline" \
		    "pmcinfo.$i.out" "pmcinfo.$i.err"
		rm -f "$fifo_a" "$fifo_b"
	done
	if [ "$successes" -eq 0 ]; then
		fail_msg="all concurrent pmcstat allocation attempts failed"
		atf_fail "$fail_msg; no allocation atomicity was exercised"
	fi
}
concurrent_process_allocations_no_residue_cleanup()
{
	pmc_atomicity_cleanup_lock
	# Keep baseline and per-iteration pmcinfo outputs under this prefix.
	rm -f start-a.*.fifo start-b.*.fifo pmcstat-a.*.out pmcstat-b.*.out \
	    pmcstat-a.*.err pmcstat-b.*.err pmcinfo*.out pmcinfo*.err
}

atf_init_test_cases()
{
	atf_add_test_case concurrent_process_allocations_no_residue
}

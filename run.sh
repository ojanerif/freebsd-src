#!/bin/sh
#
# IBS Test Suite Manager
# Comprehensive testing tool for AMD Instruction-Based Sampling (IBS)
#
# Authors: Advanced Micro Devices, Inc.
# Contact: ojanerif@amd.com
# Sponsored by AMD
#
# SPDX-License-Identifier: BSD-2-Clause
#

# Configuration
SCRIPT_DIR=$(dirname "$(realpath "$0")")
SCRIPT_VERSION="2.1.0"
REPO_URL="https://github.com/AMDESE/freebsd-src.git"
BRANCH="amdese/integration/main"
SRC_DIR="${SCRIPT_DIR}/dev/freebsd"
TESTS_DIR="${SCRIPT_DIR}/tests/sys/amd/ibs"
TESTS_INSTALL_DIR="/usr/tests/sys/amd/ibs"
SOS_DIR="./freebsd-src"
SOS_BRANCH="freebsd-ci-actions"
ARCH=$(uname -m)

# Color codes for output (disabled when stdout is not a terminal)
if [ -t 1 ]; then
    RED=$(printf '\033[0;31m')
    GREEN=$(printf '\033[0;32m')
    YELLOW=$(printf '\033[1;33m')
    BLUE=$(printf '\033[0;34m')
    PURPLE=$(printf '\033[0;35m')
    CYAN=$(printf '\033[0;36m')
    WHITE=$(printf '\033[1;37m')
    BOLD=$(printf '\033[1m')
    DIM=$(printf '\033[2m')
    NC=$(printf '\033[0m')
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; PURPLE=''; CYAN=''; WHITE=''; BOLD=''; DIM=''; NC=''
fi

# Test category and severity metadata
# get_test_meta <binary_name> → "CATEGORY_CODE:CATEGORY_LABEL:SEVERITY"
# Severities reflect pass/fail criteria from AMD IBS Testing Plan:
#   CRITICAL -any failure = overall FAIL
#   HIGH     -pass rate must be >= 95%
#   MEDIUM   -pass rate must be >= 80%
get_test_meta() {
    case "$1" in
        # TC-DET -Hardware Detection [CRITICAL]
        ibs_cpu_test)                    printf "TC-DET:Hardware Detection:CRITICAL" ;;
        ibs_detect_test)                 printf "TC-DET:Hardware Detection:CRITICAL" ;;
        # TC-MSR -MSR Control [CRITICAL]
        ibs_msr_test)                    printf "TC-MSR:MSR Control:CRITICAL" ;;
        ibs_period_test)                 printf "TC-MSR:MSR Control:CRITICAL" ;;
        # TC-INT -Interrupt Delivery [HIGH]
        ibs_interrupt_test)              printf "TC-INT:Interrupt Delivery:HIGH" ;;
        ibs_routing_test)                printf "TC-INT:Interrupt Delivery:HIGH" ;;
        # TC-DATA -Sample Accuracy [HIGH]
        ibs_data_accuracy_test)          printf "TC-DATA:Sample Accuracy:HIGH" ;;
        ibs_l3miss_test)                 printf "TC-DATA:Sample Accuracy:HIGH" ;;
        # TC-SMP -SMP/Per-CPU [HIGH]
        ibs_smp_test)                    printf "TC-SMP:SMP/Per-CPU:HIGH" ;;
        # TC-HWPMC -hwpmc API [HIGH]
        ibs_hwpmc_alloc_test)            printf "TC-HWPMC:hwpmc API:HIGH" ;;
        ibs_hwpmc_caps_test)             printf "TC-HWPMC:hwpmc API:HIGH" ;;
        ibs_hwpmc_info_test)             printf "TC-HWPMC:hwpmc API:HIGH" ;;
        ibs_hwpmc_runtime_test)          printf "TC-HWPMC:hwpmc API:HIGH" ;;
        # TC-DRV -Driver Access [HIGH]
        ibs_cpuctl_access_test)          printf "TC-DRV:Driver Access:HIGH" ;;
        # TC-CONC -Concurrency/Robustness [HIGH]
        ibs_concurrency_test)            printf "TC-CONC:Concurrency:HIGH" ;;
        ibs_robustness_test)             printf "TC-CONC:Concurrency:HIGH" ;;
        # TC-SEC -Security/Access Control [HIGH]
        ibs_access_control_test)         printf "TC-SEC:Security/Access:HIGH" ;;
        ibs_invalid_input_test)          printf "TC-SEC:Security/Access:HIGH" ;;
        # TC-API -Userspace API [MEDIUM]
        ibs_api_test)                    printf "TC-API:Userspace API:MEDIUM" ;;
        ibs_ioctl_test)                  printf "TC-API:Userspace API:MEDIUM" ;;
        ibs_swfilt_test)                 printf "TC-API:Userspace API:MEDIUM" ;;
        # TC-STR -CPU/MSR Stability Stress [MEDIUM] — Stress Batch 1 (CPU)
        ibs_stress_test)                 printf "TC-STR:Stability/Stress:MEDIUM" ;;
        ibs_cpu_stress_test)             printf "TC-STR:Stability/Stress:MEDIUM" ;;
        # TC-MEMIBS -IBS Memory Stress [MEDIUM] — Stress Batch 2 (Memory)
        ibs_mem_stress_test)             printf "TC-MEMIBS:Memory/IBS Stress:MEDIUM" ;;
        # TC-UNIT -Unit Tests (no hardware required) [MEDIUM]
        ibs_unit_cpuid_parse_test)       printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_datasrc_test)           printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_feature_flags_test)     printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_fetch_ctl_fields_test)  printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_field_masks_test)       printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_helpers_test)           printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_caps_test)              printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_ldlat_test)             printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_msr_range_test)         printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_op_data_fields_test)    printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_op_ext_maxcnt_test)     printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        ibs_unit_zen3_errata_test)       printf "TC-UNIT:Unit Tests:MEDIUM" ;;
        # TSC suite tests
        tsc_detect_test)                 printf "TC-TSC-DET:TSC Detection:CRITICAL" ;;
        tsc_drift_test)                  printf "TC-TSC-DRF:TSC Drift:HIGH" ;;
        tsc_invariant_test)              printf "TC-TSC-INV:TSC Invariant:HIGH" ;;
        tsc_stress_test)                 printf "TC-TSCSTR:TSC Stress:HIGH" ;;
        # PMC suite tests
        hwpmc_exterr_test)               printf "TC-PMCAPI:hwpmc API:HIGH" ;;
        hwpmc_grouping_test)             printf "TC-PMCAPI:hwpmc API:HIGH" ;;
        pmcstat_group_alloc_atomicity_test) printf "TC-PMCSTAT:pmcstat Integration:HIGH" ;;
        pmcstat_grouping_test)           printf "TC-PMCSTAT:pmcstat Integration:MEDIUM" ;;
        pmcstat_ibs_errata_test)         printf "TC-PMCSTAT:pmcstat Decode:HIGH" ;;
        pmcstat_tsc_test)                printf "TC-PMCSTAT:pmcstat Decode:HIGH" ;;
        # UMCDF tests
        umcdf_cpuid_test)                printf "TC-UMCDET:UMC/DF Detection:CRITICAL" ;;
        umcdf_df_test)                   printf "TC-UMCPMC:DF PMC:HIGH" ;;
        umcdf_umc_test)                  printf "TC-UMCPMC:UMC PMC:HIGH" ;;
        umcdf_unit_capabilities_test)    printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        umcdf_unit_df_config_dispatch_test) printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        umcdf_unit_df_encoding_test)     printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        umcdf_unit_perfmonv2_test)       printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        umcdf_unit_vendor_test)          printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        umcdf_unit_zen_map_test)         printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        umcdf_unit_zen_name_test)        printf "TC-UMCUNIT:UMCDF Unit:MEDIUM" ;;
        # L3 suite tests
        l3_detect_test)                  printf "TC-DET-L3:L3 Detection:CRITICAL" ;;
        l3_hit_test)                     printf "TC-UNC-L3:L3 PMC:HIGH" ;;
        l3_miss_test)                    printf "TC-UNC-L3:L3 PMC:HIGH" ;;
        # STRESS suite tests
        cpu_stress_test)                 printf "TC-CSTR:CPU Stress:MEDIUM" ;;
        mem_stress_test)                 printf "TC-MSTR:Memory Stress:MEDIUM" ;;
        net_stress_test)                 printf "TC-NSTR:Network Stress:MEDIUM" ;;
        disk_stress_test)                printf "TC-DSTR:Disk Stress:MEDIUM" ;;
        ibs_op_stress_test)              printf "TC-ISTR:IBS-Op Stress:MEDIUM" ;;
        ibs_fetch_stress_test)           printf "TC-FSTR:IBS-Fetch Stress:MEDIUM" ;;
        # TC-NMISTR -NMI Stress [HIGH] — Stress Batch 1 (CPU); was TC-INT
        ibs_nmi_stress_test)             printf "TC-NMISTR:NMI Stress:HIGH" ;;
        *)                               printf "TC-MISC:Miscellaneous:MEDIUM" ;;
    esac
}

# Returns ANSI color for a severity level
severity_color() {
    case "$1" in
        CRITICAL) printf '%s' "$RED" ;;
        HIGH)     printf '%s' "$YELLOW" ;;
        MEDIUM)   printf '%s' "$CYAN" ;;
        *)        printf '%s' "$WHITE" ;;
    esac
}

# Read a counter temp file as an unsigned decimal.  Treat missing, empty, or
# corrupted content as zero so report printf/test operands stay numeric.
read_counter_file() {
    _rcf_value=$(cat "$1" 2>/dev/null || printf '')
    case "$_rcf_value" in
        ''|*[!0123456789]*) printf '0' ;;
        *)                  printf '%s' "$_rcf_value" ;;
    esac
}

# Detect CPU vendor string (FreeBSD sysctl)
get_cpu_vendor() {
    sysctl -n hw.cpu_vendor 2>/dev/null || \
        sysctl -n hw.model 2>/dev/null | awk '{print $1}' || \
        echo "Unknown"
}

# Print CPU feature-bit context; on non-AMD hardware every IBS CPUBIT is off.
# Format mirrors what a tester would note manually: "CPUBIT <name> [CPUID leaf]
# = off -<reason>".
print_cpu_test_context() {
    VENDOR=$(get_cpu_vendor)
    MODEL=$(sysctl -n hw.model 2>/dev/null || echo "Unknown")

    UNAME=$(uname -a)

    echo ""
    echo "================================================================="
    echo "CPU TEST CONTEXT"
    printf "  Vendor : %s\n" "$VENDOR"
    printf "  Model  : %s\n" "$MODEL"
    printf "  uname  : %s\n" "$UNAME"
    echo "-----------------------------------------------------------------"

    case "$VENDOR" in
        AuthenticAMD|AMD)
            printf "  ${GREEN}AMD CPU detected -hardware IBS feature bits active${NC}\n"
            printf "  CPUBIT IBS_SUPPORT   [CPUID 8000_0001h ECX bit 10] = ${GREEN}on${NC}\n"
            printf "  CPUBIT IbsFetchEnable [MSR C001_1030h bit 18]       = ${GREEN}on${NC} (HW)\n"
            printf "  CPUBIT IbsOpEnable    [MSR C001_1033h bit 19]       = ${GREEN}on${NC} (HW)\n"
            ;;
        *)
            printf "  ${YELLOW}Non-AMD CPU -IBS is an AMD-exclusive feature.${NC}\n"
            printf "  Tests ran without hardware IBS support; relevant feature bits:\n"
            echo ""
            printf "  CPUBIT IBS_SUPPORT    [CPUID 8000_0001h ECX bit 10] = ${RED}off${NC}  (not AMD)\n"
            printf "  CPUBIT IbsFetchMaxCnt [MSR C001_1030h bits 15:0]    = ${RED}off${NC}  (no HW)\n"
            printf "  CPUBIT IbsFetchEnable [MSR C001_1030h bit 18]       = ${RED}off${NC}  (no HW)\n"
            printf "  CPUBIT IbsFetchVal    [MSR C001_1031h bit 49]       = ${RED}off${NC}  (no HW)\n"
            printf "  CPUBIT IbsOpEnable    [MSR C001_1033h bit 19]       = ${RED}off${NC}  (no HW)\n"
            printf "  CPUBIT IbsOpVal       [MSR C001_1035h bit 18]       = ${RED}off${NC}  (no HW)\n"
            echo ""
            printf "  ${YELLOW}[WARNING]${NC} CRITICAL/HIGH tests that require AMD IBS hardware\n"
            printf "  will fail or be skipped on this platform. Results below\n"
            printf "  reflect software/emulation paths only.\n"
            ;;
    esac

    echo "================================================================="
    echo ""
}

# Global variables
VERBOSE=0
DRY_RUN=0
FORCE=0
PARALLELISM="${PARALLELISM:-$(sysctl -n hw.ncpu 2>/dev/null || echo 1)}"
HTML_DIR="${HTML_DIR:-/usr/local/www/darkhttpd}"
RESULTS_DIR="${RESULTS_DIR:-$HTML_DIR/results-$(date +%Y-%m-%d_%H-%M-%S)}"
TEST_RESULTS=""
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0
XFAIL_TESTS=0
BROKEN_TESTS=0

# Suite / category selection
# SUITE: IBS | UMCDF | PMC | TSC | L3 | STRESS | ALL | DEFAULT
# DEFAULT (no --suite flag) expands to: IBS UMCDF PMC
# ALL expands to: IBS UMCDF PMC TSC L3 STRESS
SUITE="${SUITE:-DEFAULT}"
# CATEGORIES: space-separated list of TC-* codes; empty = all categories for the suite
CATEGORIES=""

# --stress: launch background stressors (cpu/mem/net/disk) while the primary suite runs.
# WARNING: combining --stress with IBS tests at high sampling rates can trigger the NMI
# overflow crash described in FreeBSD-Tests-026.  Use --safe-ibs-rate to cap the period.
WITH_STRESS=0
SAFE_IBS_RATE=""	# if non-empty, written to dev.hwpmc.ibs.min_period before the run
STRESS_PIDS=""		# PIDs of background stressor processes (space-separated)
_STRESS_MON_PID=""	# PID of the in-process stress_monitor_loop (cleanup target)

# --email: set to 1 when --email flag is given explicitly; --run-all sends a report only
# when this is 1.  --auto always emails regardless.
EMAIL_REQUESTED=0

# --auto mode
AUTO_MODE=0
REPORT_EMAIL="${REPORT_EMAIL:-freebsd-test@mailman-svr.amd.com,ojanerif@amd.com}"   # comma-separated for multiple recipients
SENDER_EMAIL="${SENDER_EMAIL:-freebsd-ci-actions@amd.com}"
AUTO_KERNCONF="${AUTO_KERNCONF:-GENERIC}"
AUTOTEST_SENTINEL="/var/db/ibs-autotest-sentinel"
LAST_COMMIT_FILE="/var/db/ibs-autotest-last-commit"
FORCE_REBUILD=0	# --force-rebuild: skip the "already tested" commit check in --auto
RCD_SERVICE="/usr/local/etc/rc.d/ibs_autotest"

# Panic-recovery / --last-test persistent files.
# Written during every --run-all so that after a kernel panic + reboot we
# can see exactly which test was running when the system went down.
LAST_RUN_LOG="/var/log/ibs-last-run.log"   # full raw kyua output, appended per line
LAST_TEST_STATE="/var/log/ibs-last-test.state"  # last completed test + timestamp

# Remove temp files on exit or interrupt (SIGINT/SIGTERM)
_ibs_cleanup() {
    # Kill any background stressor processes before cleaning temp files.
    if [ -n "$STRESS_PIDS" ]; then
        # shellcheck disable=SC2086
        kill -TERM $STRESS_PIDS 2>/dev/null || true
    fi
    # Kill stress monitor loop if still alive (panic/SIGINT path).
    if [ -n "${_STRESS_MON_PID:-}" ]; then
        kill -TERM "$_STRESS_MON_PID" 2>/dev/null || true
    fi
    rm -f /tmp/ibs_exit_$$.tmp /tmp/ibs_pass_$$.tmp /tmp/ibs_fail_$$.tmp \
          /tmp/ibs_skip_$$.tmp /tmp/ibs_xfail_$$.tmp /tmp/ibs_broken_$$.tmp \
          /tmp/ibs_crit_f_$$.tmp /tmp/ibs_high_p_$$.tmp /tmp/ibs_high_f_$$.tmp \
          /tmp/ibs_med_p_$$.tmp /tmp/ibs_med_f_$$.tmp /tmp/ibs_matrix_$$.tmp \
          /tmp/ibs_kyuafile_$$.tmp /tmp/ibs_desc_$$.tmp \
          /tmp/ibs_sm_done_$$.tmp /tmp/ibs_sm_last_$$.tmp \
          /tmp/ibs_idx_$$.tmp /tmp/ibs_exit_$$_*.tmp 2>/dev/null || true
}
trap _ibs_cleanup EXIT INT TERM

# ── Suite helpers ──────────────────────────────────────────────────────────

# Return the source (build) directory for a suite
suite_src_dir() {
    case "$1" in
        DEFAULT) printf '%s' "${SCRIPT_DIR}/tests/sys/amd/ibs" ;;
        IBS)    printf '%s' "${SCRIPT_DIR}/tests/sys/amd/ibs" ;;
        UMCDF)  printf '%s' "${SCRIPT_DIR}/tests/sys/amd/umcdf" ;;
        PMC)    printf '%s' "${SCRIPT_DIR}/tests/sys/amd/pmc" ;;
        TSC)    printf '%s' "${SCRIPT_DIR}/tests/sys/amd/tsc" ;;
        L3)     printf '%s' "${SCRIPT_DIR}/tests/sys/amd/l3" ;;
        STRESS) printf '%s' "${SCRIPT_DIR}/tests/sys/amd/stress" ;;
        *)      return 1 ;;
    esac
}

# Return the install (kyua) directory for a suite
suite_install_dir() {
    case "$1" in
        DEFAULT) printf '/usr/tests/sys/amd/ibs' ;;
        IBS)    printf '/usr/tests/sys/amd/ibs' ;;
        UMCDF)  printf '/usr/tests/sys/amd/umcdf' ;;
        PMC)    printf '/usr/tests/sys/amd/pmc' ;;
        TSC)    printf '/usr/tests/sys/amd/tsc' ;;
        L3)     printf '/usr/tests/sys/amd/l3' ;;
        STRESS) printf '/usr/tests/sys/amd/stress' ;;
        *)      return 1 ;;
    esac
}

# Expand a SUITE selector to a space-separated list of individual suites.
expand_suite_list() {
    case "$1" in
        IBS|UMCDF|PMC|TSC|L3|STRESS) printf '%s' "$1" ;;
        ALL)     printf 'IBS UMCDF PMC TSC L3 STRESS' ;;
        DEFAULT) printf 'IBS UMCDF PMC' ;;
        *)       return 1 ;;
    esac
}

# ── Background stressor management ─────────────────────────────────────────

# Start the four stressor binaries as background processes.
# Sets STRESS_PIDS to the space-separated list of PIDs.
# Called by run_all_tests() when WITH_STRESS=1.
start_background_stressors() {
    _stress_dir='/usr/tests/sys/amd/stress'

    if [ ! -d "$_stress_dir" ]; then
        log_error "Stressor binaries not found in ${_stress_dir}."
        log_error "Run: ./run.sh --suite STRESS --compile first."
        exit 1
    fi

    # Safety warning when combining with suites that exercise IBS at high rates.
    case "$SUITE" in
        IBS|ALL)
            printf '%s\n' ""
            printf '%s[WARNING] --stress + %s suite:%s\n' "$RED" "$SUITE" "$NC"
            printf '  Combining background stressors with IBS tests at high\n'
            printf '  NMI rates can reproduce the crash described in\n'
            printf '  FreeBSD-Tests-026 (NMI storm -> network stack panic).\n'
            printf '  The net stressor runs at reduced intensity\n'
            printf '  (NET_STRESS_THREADS=2).  Use --safe-ibs-rate N to cap\n'
            printf '  the minimum IBS sampling period before the run.\n'
            printf '%s\n' ""
            ;;
    esac

    # Apply safe IBS rate cap if requested.
    if [ -n "$SAFE_IBS_RATE" ]; then
        log_info "Applying safe IBS rate cap: dev.hwpmc.ibs.min_period=${SAFE_IBS_RATE}"
        sysctl "dev.hwpmc.ibs.min_period=${SAFE_IBS_RATE}" 2>/dev/null || \
            log_warning "Could not set dev.hwpmc.ibs.min_period (sysctl not present yet)"
    fi

    # Net stressor runs at reduced thread count when used as background load.
    NET_STRESS_THREADS=2
    export NET_STRESS_THREADS

    log_info "Starting background stressors (stress dir: ${_stress_dir})..."
    STRESS_PIDS=""
    for _s in cpu_stressor mem_stressor net_stressor disk_stressor; do
        if [ ! -x "${_stress_dir}/${_s}" ]; then
            log_warning "Stressor binary not found: ${_stress_dir}/${_s} (skipping)"
            continue
        fi
        "${_stress_dir}/${_s}" &
        _pid=$!
        STRESS_PIDS="${STRESS_PIDS} ${_pid}"
        log_verbose "Started ${_s} (pid ${_pid})"
    done

    log_info "Background stressors running (PIDs:${STRESS_PIDS})"
    log_info "Waiting 2 s for stressors to reach steady state..."
    sleep 2
}

# Stop all background stressor processes started by start_background_stressors().
stop_background_stressors() {
    [ -z "$STRESS_PIDS" ] && return 0
    log_info "Stopping background stressors (PIDs:${STRESS_PIDS})..."
    # shellcheck disable=SC2086
    for _pid in $STRESS_PIDS; do
        kill -TERM "$_pid" 2>/dev/null || true
    done
    sleep 1
    # shellcheck disable=SC2086
    for _pid in $STRESS_PIDS; do
        wait "$_pid" 2>/dev/null || true
    done
    STRESS_PIDS=""
    log_info "Background stressors stopped"
}

# ── Stress suite console monitor ────────────────────────────────────────────

# Background loop: every 10 s write a two-line resource snapshot to /dev/tty
# (not stdout) so it appears on the console without polluting the kyua pipe.
# Args: start_epoch  total_tests  done_count_file  last_test_file
stress_monitor_loop() {
    _sm_start="$1"; _sm_total="$2"; _sm_done_f="$3"; _sm_last_f="$4"
    _sm_ps=$(sysctl -n hw.pagesize 2>/dev/null || echo 4096)
    _sm_total_pages=$(sysctl -n vm.stats.vm.v_page_count 2>/dev/null || echo 0)
    _sm_total_mb=$(( _sm_total_pages * _sm_ps / 1048576 ))

    while true; do
        sleep 10
        _sm_now=$(date +%s)
        _sm_el=$(( _sm_now - _sm_start ))
        _sm_done=$(cat "$_sm_done_f" 2>/dev/null || echo 0)
        _sm_last=$(cat "$_sm_last_f" 2>/dev/null || echo "(none yet)")

        # ETA: pro-rate elapsed time; fall back to 120 s/test if nothing done yet
        if [ "$_sm_done" -gt 0 ]; then
            _sm_rem=$(( _sm_el / _sm_done * (_sm_total - _sm_done) ))
        else
            _sm_rem=$(( _sm_total * 120 ))
        fi
        [ "$_sm_rem" -lt 0 ] && _sm_rem=0

        # CPU load average (1 / 5 / 15 min)
        _sm_load=$(sysctl -n vm.loadavg 2>/dev/null | awk '{printf "%s %s %s", $2, $3, $4}')

        # Free + used memory
        _sm_fp=$(sysctl -n vm.stats.vm.v_free_count 2>/dev/null || echo 0)
        _sm_free_mb=$(( _sm_fp * _sm_ps / 1048576 ))
        _sm_used_mb=$(( _sm_total_mb - _sm_free_mb ))

        # /tmp available (KiB → MiB)
        _sm_df=$(df -k /tmp 2>/dev/null | awk 'NR==2 {printf "%d MiB avail", int($4/1024)}')

        # lo0 cumulative bytes (link-level row)
        _sm_lo=$(netstat -ibn 2>/dev/null | \
            awk '$1=="lo0" && $3~/<Link/{printf "rx=%s tx=%s", $7, $10}')

        printf '\n  [STRESS %ds / ~%ds | %d/%d done | running: %s]\n  [CPU load: %s | mem: %d/%d MiB used | /tmp: %s | lo0: %s]\n' \
            "$_sm_el" "$_sm_rem" "$_sm_done" "$_sm_total" "$_sm_last" \
            "$_sm_load" "$_sm_used_mb" "$_sm_total_mb" "$_sm_df" "${_sm_lo:-(n/a)}" \
            > /dev/tty
    done
}

# ── Per-test diagnostic table ──────────────────────────────────────────────
#
# diag_for_test TESTID
#
# Prints four lines to stdout:
#   CLASS:      normal | actionable
#   OPERATION:  one-line description of what the test does
#   CAUSE:      why it skipped / failed
#   ACTION:     what to do about it (empty for "normal" skips)
#
# "normal" = hardware/version mismatch — skip is expected, no action needed.
# "actionable" = missing kernel feature, patch, or config — needs follow-up.
#
diag_for_test() {
    _dt_id="$1"
    case "$_dt_id" in

    # ── IBS NMI stress ────────────────────────────────────────────────────
    ibs_nmi_stress_test:ibs_nmi_drain_under_load)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Executes AMD Erratum #420 workaround drain sequence 100 times under full CPU stress (192 threads): clear MaxCnt, clear IbsOpEn, then 50x1us zero-writes to the MSR per iteration; asserts each drain completes within 5000 us.\n'
        printf 'CAUSE:4 of 100 drain iterations exceeded the 5000 us latency threshold. Under maximum CPU load the NMI handler contention slows the drain sequence beyond the expected bound.\n'
        printf 'ACTION:Investigate NMI handler re-arm contention under 192-CPU load; consider raising the threshold for high-core-count machines or pinning the drain loop to an isolated CPU.\n'
        ;;

    ibs_nmi_stress_test:ibs_nmi_rate_limit_enforce)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Programs an IBS Op period below the kernel-enforced minimum and verifies the kernel clamps it to the minimum value via sysctl dev.hwpmc.ibs.min_period.\n'
        printf 'CAUSE:sysctl dev.hwpmc.ibs.min_period does not exist in this kernel; the IBS rate-limiting patch (FreeBSD-Tests-026) has not been applied.\n'
        printf 'ACTION:Apply kernel patch FreeBSD-Tests-026, rebuild the kernel, and re-run.\n'
        ;;

    # ── IBS robustness ────────────────────────────────────────────────────
    ibs_robustness_test:ibs_robustness_nmi_flood)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Enables IBS Op with MaxCnt=1 (16-cycle period, ~250M NMIs/s at 4 GHz) to stress-test NMI delivery robustness on a single CPU.\n'
        printf 'CAUSE:Placeholder — the hwpmc IBS NMI handler is not yet registered in this kernel build. Without a registered handler the system would lock up on the first unhandled NMI.\n'
        printf 'ACTION:Register the IBS NMI handler in hwpmc before enabling; see TODO.md TC-ROB-01.\n'
        ;;

    ibs_robustness_test:ibs_robustness_all_cpus_nmi_flood)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Same as TC-ROB-01 but enables MaxCnt=1 IBS Op sampling simultaneously on every CPU to test system-wide NMI storm resilience.\n'
        printf 'CAUSE:Placeholder — same prerequisite as TC-ROB-01: hwpmc IBS NMI handler must be registered first.\n'
        printf 'ACTION:Register the IBS NMI handler in hwpmc before enabling; see TODO.md TC-ROB-02.\n'
        ;;

    # ── IBS routing ───────────────────────────────────────────────────────
    ibs_routing_test:ibs_ibsctl_global)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Writes MSR_AMD64_IBSCTL (0xc001103a) to set the global IBSCTL_FETCH_EN and IBSCTL_OP_EN bits, then reads back to verify the write took effect.\n'
        printf 'CAUSE:The write to MSR_AMD64_IBSCTL returned an error (Bad address / EIO). The MSR requires elevated kernel privilege that is not present, or cpuctl.ko is not exposing write access to this register.\n'
        printf 'ACTION:Verify cpuctl.ko is loaded and the process has the required capability; check if IBSCTL is accessible on this BIOS/firmware configuration.\n'
        ;;

    # ── IBS detection ─────────────────────────────────────────────────────
    ibs_detect_test:ibs_detect_msr_access)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Reads MSR_AMD64_IBSOPDATA4 (0xc001103d) to verify Zen4+ IBS Op extended data is accessible; this register is only populated during active IBS Op sampling.\n'
        printf 'CAUSE:Read of MSR_AMD64_IBSOPDATA4 returned Bad address without active IBS Op sampling running. The register is read-only and only valid while the IBS Op engine is counting.\n'
        printf 'ACTION:Start IBS Op sampling before reading IBSOPDATA4, or restructure the test to enable sampling first; alternatively confirm the kernel exposes IBSOPDATA4 reads via cpuctl.\n'
        ;;

    # ── IBS CPU detection — hardware-specific (normal skips) ─────────────
    ibs_cpu_test:ibs_cpu_zen5_detection)
        printf 'CLASS:normal\n'
        printf 'OPERATION:Detects AMD Zen5 (Family 0x1a) CPU via CPUID and validates Zen5-specific IBS feature bits.\n'
        printf 'CAUSE:CPU is Family 0x19 (Zen 4) — not a Zen5 processor. Skip is expected on this hardware.\n'
        printf 'ACTION:\n'
        ;;

    ibs_cpu_test:ibs_cpu_tsc_frequency)
        printf 'CLASS:normal\n'
        printf 'OPERATION:Determines TSC frequency using CPUID leaf 0x15 (TSC/core crystal clock ratio) or 0x16 (processor frequency info).\n'
        printf 'CAUSE:AMD EPYC 9654 (Zen 4) does not expose CPUID leaf 0x15 or 0x16 with valid data. Skip is expected on this CPU family.\n'
        printf 'ACTION:\n'
        ;;

    # ── IBS data accuracy — Zen4+ MSR (normal on this stepping) ──────────
    ibs_data_accuracy_test:ibs_op_data4_zen4)
        printf 'CLASS:normal\n'
        printf 'OPERATION:Validates IBS Op Data 4 fields (remote latency, L3 miss data) by writing and reading back test values via MSR_AMD64_IBSOPDATA4 on a Zen4+ CPU.\n'
        printf 'CAUSE:MSR_AMD64_IBSOPDATA4 returned Bad address on this CPU/stepping even though the CPU family is Zen4; the MSR may require sampling to be active or is not accessible via cpuctl on this build.\n'
        printf 'ACTION:\n'
        ;;

    # ── IBS ioctl ─────────────────────────────────────────────────────────
    ibs_ioctl_test:ibs_ioctl_not_implemented)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Placeholder for IBS-specific ioctl API validation against /dev/cpuctl.\n'
        printf 'CAUSE:The IBS ioctl interface has not been implemented in the FreeBSD hwpmc kernel module yet.\n'
        printf 'ACTION:Implement the IBS ioctl handler in sys/dev/hwpmc/hwpmc_amd.c and wire it to /dev/cpuctl.\n'
        ;;

    # ── IBS L3MissOnly — normal skip (Zen4+ correctly skips this) ─────────
    ibs_l3miss_test:ibs_l3miss_disabled_on_older)
        printf 'CLASS:normal\n'
        printf 'OPERATION:Verifies that the L3MissOnly filter bit (IBS_FETCH_L3_MISS_ONLY, bit 59) is not retained on pre-Zen4 CPUs by writing it and confirming it reads back as 0.\n'
        printf 'CAUSE:CPU supports L3MissOnly (Zen4+), so the test guards against false positives and skips intentionally on capable hardware.\n'
        printf 'ACTION:\n'
        ;;

    # ── IBS swfilt — bare-metal normal skip ───────────────────────────────
    ibs_swfilt_test:ibs_swfilt_exclude_hv)
        printf 'CLASS:normal\n'
        printf 'OPERATION:Tests the hypervisor-exclude software filter bit in IBS Fetch Control by enabling sampling, running HV-mode instructions, and verifying HV samples are excluded.\n'
        printf 'CAUSE:System is running on bare metal (kern.vm_guest=none). The hypervisor-exclude filter is only meaningful inside a guest VM. Skip is expected.\n'
        printf 'ACTION:\n'
        ;;

    # ── IBS stress — kernel bug skip ──────────────────────────────────────
    ibs_stress_test:ibs_stress_period_changes)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Performs 500 rapid IBS Op period changes while sampling is disabled (IbsOpEn=0), verifying the MaxCnt field is preserved across each write/read cycle.\n'
        printf 'CAUSE:Known kernel bug FreeBSD-Tests-010: the hwpmc NMI handler re-arms the IBS counter with its stored period even when IbsOpEn=0, corrupting the MaxCnt field and causing the test to skip to avoid a false failure.\n'
        printf 'ACTION:Fix the kernel NMI handler to skip counter re-arm when IbsOpEn=0; tracked as FreeBSD-Tests-010.\n'
        ;;

    # ── IBS hwpmc alloc — IBS backend not implemented ─────────────────────
    ibs_hwpmc_alloc_test:ibs_hwpmc_alloc_fetch_basic)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Calls pmc_allocate() with PMC_CLASS_IBS / IBS_FETCH mode and asserts it succeeds, verifying the hwpmc IBS Fetch backend accepts a basic allocation request.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP — the IBS PMC backend (PMC_CLASS_IBS) is not implemented in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Implement the IBS Fetch allocation path in sys/dev/hwpmc/hwpmc_amd.c and expose it via libpmc.\n'
        ;;

    ibs_hwpmc_alloc_test:ibs_hwpmc_alloc_op_basic)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Calls pmc_allocate() with PMC_CLASS_IBS / IBS_OP mode and asserts it succeeds, verifying the hwpmc IBS Op backend accepts a basic allocation request.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP — the IBS PMC backend (PMC_CLASS_IBS) is not implemented in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Implement the IBS Op allocation path in sys/dev/hwpmc/hwpmc_amd.c and expose it via libpmc.\n'
        ;;

    ibs_hwpmc_alloc_test:ibs_hwpmc_alloc_reject_counting_mode)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Requests an IBS PMC in non-sampling (counting) mode and asserts pmc_allocate() rejects it with EINVAL, since IBS is sampling-only.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP before it could check the mode argument — the IBS backend is not implemented in this tree, so the rejection reason is wrong.\n'
        printf 'ACTION:Implement the IBS backend; the mode-validation rejection will then work correctly.\n'
        ;;

    ibs_hwpmc_alloc_test:ibs_hwpmc_alloc_reject_bad_rate)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Requests an IBS PMC with a sampling rate below the hardware minimum and asserts pmc_allocate() rejects it with EINVAL.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP before reaching rate validation — the IBS backend is not implemented in this tree.\n'
        printf 'ACTION:Implement the IBS backend; rate-limit validation will then be reachable.\n'
        ;;

    ibs_hwpmc_alloc_test:ibs_hwpmc_alloc_reject_unknown_qualifier)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Requests an IBS PMC with an unknown qualifier flag set and asserts pmc_allocate() rejects it with EINVAL.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP before reaching qualifier validation — the IBS backend is not implemented in this tree.\n'
        printf 'ACTION:Implement the IBS backend; qualifier validation will then be reachable.\n'
        ;;

    # ── IBS hwpmc caps — IBS backend not implemented ──────────────────────
    ibs_hwpmc_caps_test:ibs_hwpmc_fetch_caps_and_width)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Calls pmc_getcpuinfo() and inspects the PMC_CLASS_IBS class descriptor: verifies that at least one IBS Fetch row is reported and that the counter width is non-zero.\n'
        printf 'CAUSE:PMC_CLASS_IBS is absent from the pmc_cpuinfo class list — the IBS backend is not registered in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Register PMC_CLASS_IBS in the hwpmc capabilities table and expose it via libpmc pmc_getcpuinfo().\n'
        ;;

    ibs_hwpmc_caps_test:ibs_hwpmc_pmcinfo_rows_present)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Calls pmc_getpmcinfo() and verifies that at least two IBS rows (one Fetch, one Op) are present in the per-CPU PMC row table.\n'
        printf 'CAUSE:No IBS rows appear in pmc_getpmcinfo() — the IBS backend is not registered in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Register PMC_CLASS_IBS rows in hwpmc and expose them via libpmc pmc_getpmcinfo().\n'
        ;;

    # ── IBS hwpmc info — IBS backend not implemented ──────────────────────
    ibs_hwpmc_info_test:ibs_hwpmc_info_class_present)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Calls pmc_getcpuinfo() and asserts that PMC_CLASS_IBS appears in the returned class list, confirming the IBS event class is visible to userspace.\n'
        printf 'CAUSE:PMC_CLASS_IBS is absent from the pmc_cpuinfo class list — the IBS backend is not registered in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Register PMC_CLASS_IBS in the hwpmc capabilities table and expose it via libpmc pmc_getcpuinfo().\n'
        ;;

    ibs_hwpmc_info_test:ibs_hwpmc_info_class_name_visible)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Iterates pmc_getcpuinfo() class descriptors and asserts the class named "IBS" is present, verifying the human-readable class name is correct.\n'
        printf 'CAUSE:No class named "IBS" found in pmc_getcpuinfo() output — the IBS backend is not registered in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Register PMC_CLASS_IBS with the correct name string in the hwpmc class table.\n'
        ;;

    ibs_hwpmc_info_test:ibs_hwpmc_info_event_names_visible)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Calls pmc_event_names_of_class(PMC_CLASS_IBS) and asserts at least two event names are returned (e.g. IBS-FETCH, IBS-OP), confirming the event table is populated.\n'
        printf 'CAUSE:pmc_event_names_of_class(PMC_CLASS_IBS) returned no events or failed — the IBS backend and its event table are not registered in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Populate the IBS event table in hwpmc_amd.c and wire it into libpmc event name resolution.\n'
        ;;

    # ── IBS hwpmc runtime — IBS backend not implemented ───────────────────
    ibs_hwpmc_runtime_test:ibs_hwpmc_fetch_lifecycle_smoke)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates an IBS Fetch PMC via pmc_allocate(), attaches it to the current process, starts sampling, lets it run briefly, stops it, and releases it — exercising the full allocate→attach→start→stop→free lifecycle.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP at the first step — the IBS PMC runtime backend is not implemented in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Implement the IBS Fetch sampling lifecycle (allocate, attach, start, stop, free) in sys/dev/hwpmc/hwpmc_amd.c.\n'
        ;;

    ibs_hwpmc_runtime_test:ibs_hwpmc_getmsr_virtual_negative)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates an IBS Op PMC and calls pmc_rw() with a virtual MSR index out of the valid IBS MSR range, asserting that EINVAL is returned — verifying bounds checking on the virtual MSR accessor.\n'
        printf 'CAUSE:pmc_allocate() returned EOPNOTSUPP before the MSR accessor was reached — the IBS PMC backend is not implemented in this libpmc/hwpmc tree.\n'
        printf 'ACTION:Implement the IBS backend; the virtual MSR bounds check will then be exercisable.\n'
        ;;

    # ── hwpmc_grouping — PMC row disposition API ───────────────────────────
    hwpmc_grouping_test:row_disposition_tracks_process_and_system_pmc)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates one SOFT-CLOCK.HARD PMC in thread-counting mode (TC) and one in system-counting mode (SC), then calls pmc_getpmcinfo() to verify the row disposition is PMC_DISP_THREAD for the process PMC and PMC_DISP_STANDALONE for the system PMC.\n'
        printf 'CAUSE:pmc_allocate() or pmc_getpmcinfo() failed, or the disposition values returned do not match expectations — possibly because the row-disposition field (PMC_DISP_*) is not implemented or not exposed in this libpmc/hwpmc version.\n'
        printf 'ACTION:Confirm pmc_getpmcinfo() populates the disposition field and that PMC_DISP_THREAD / PMC_DISP_STANDALONE are defined; check dmesg for hwpmc initialization errors.\n'
        ;;

    hwpmc_grouping_test:amd_pmu_core_events_allocate_concurrently)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates the portable "unhalted-cycles" AMD Zen core PMU event twice as independent process-counting (TC) hwpmc rows and asserts both get distinct row numbers, verifying the PMU grouping logic does not force them onto the same row.\n'
        printf 'CAUSE:pmc_allocate() failed or the two PMCs share the same row — either PMU JSON tables for the running CPU are absent, or the grouping logic incorrectly aliases the two allocations.\n'
        printf 'ACTION:Verify PMU JSON tables for the AMD CPU family are present and that amd_umcdf_skip_unless_pmu_events() does not guard-skip; inspect hwpmc grouping logic for concurrent-allocation bugs.\n'
        ;;

    hwpmc_grouping_test:amd_pmu_mixed_core_events_allocate_concurrently)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates "unhalted-cycles" and "instructions" as distinct process-counting hwpmc rows and asserts they receive different row numbers, covering mixed-event concurrent allocation.\n'
        printf 'CAUSE:pmc_allocate() failed for one or both events, or the two events share a row — either PMU JSON tables are absent or the grouping logic incorrectly merges different event types.\n'
        printf 'ACTION:Verify PMU JSON tables expose both "unhalted-cycles" and "instructions" for this CPU; inspect hwpmc grouping for cross-event aliasing bugs.\n'
        ;;

    hwpmc_grouping_test:system_sampling_requires_logfile_before_start)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates SOFT-CLOCK.HARD in system-sampling mode (SS) without configuring a log file, then calls pmc_start() and asserts it returns EDOOFUS — verifying the kernel enforces logfile-before-start ordering.\n'
        printf 'CAUSE:pmc_allocate() or pmc_start() returned an unexpected error, or pmc_start() succeeded (no EDOOFUS) — possibly because the logfile enforcement check is absent or returns a different errno in this hwpmc version.\n'
        printf 'ACTION:Check hwpmc for the EDOOFUS guard on pmc_start() with system-sampling PMCs that lack a log file; confirm the behavior matches the ATF assertion.\n'
        ;;

    # ── UMCDF unit — DF config dispatch ───────────────────────────────────
    umcdf_unit_df_config_dispatch_test:umcdf_unit_dfdis_df1_df2_differ)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Constructs a DF event with event_code=0x0107 and umask=0x55, computes amd_umcdf_expected_df_config() for both Zen3 (DF1 format) and Zen4 (DF2 format), and asserts the two results differ — validating that the high-nibble event-code encoding diverges between DF generations.\n'
        printf 'CAUSE:amd_umcdf_expected_df_config() returned identical values for Zen3 and Zen4, indicating the DF1/DF2 encoding branches produce the same bit pattern for this event code — either the high-nibble placement logic is missing or uses the same formula for both generations.\n'
        printf 'ACTION:Review amd_umcdf_expected_df_config() in the umcdf library: confirm DF1 encodes bits[11:8] of event_code differently from DF2 (e.g. DF1 uses a dedicated high-nibble field vs DF2 extended-event layout).\n'
        ;;

    # ── UMCDF unit — capabilities ─────────────────────────────────────────
    umcdf_unit_capabilities_test:umcdf_unit_cap_df1_df2_differ)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Constructs event_code=0x0107 / umask=0x01, calls amd_umcdf_expected_df_config() for Zen3 (DF1) and Zen4 (DF2), and asserts df1_cfg != df2_cfg — verifying the capabilities layer routes to the correct generation-specific encoder.\n'
        printf 'CAUSE:amd_umcdf_expected_df_config() produced the same 64-bit config word for both Zen3 and Zen4 with event 0x0107 — the capabilities dispatch is either missing the generation split or calling the same encoder for both.\n'
        printf 'ACTION:Inspect the amd_umcdf_expected_df_config() dispatch path: confirm it selects DF1 encoding for AMD_UMCDF_ZEN_3 and DF2 encoding for AMD_UMCDF_ZEN_4, and that the two encoders produce distinct results for events with bits[11:8] set.\n'
        ;;

    # ── pmcstat IBS errata — Zen 3 B0 icmiss suppression ─────────────────
    pmcstat_ibs_errata_test:zen3_b0_fetch_icmiss_suppressed)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Writes a synthetic pmclog stream with IBS Fetch records carrying the IbsIcMiss bit set, stamped with Zen 3 B0 producer CPUIDs (AuthenticAMD-25-00-1 and AuthenticAMD-25-0F-1), then runs pmcstat -R and asserts the icmiss token is absent from the decoded Fetch line — verifying erratum #1238 suppression.\n'
        printf 'CAUSE:pmcstat -R printed "icmiss" in the decoded output for a Zen 3 B0 CPUID even though the erratum #1238 workaround should suppress it — either the CPUID range check in pmcstat is wrong, missing, or the workaround was not applied to the patched pmcstat binary under test.\n'
        printf 'ACTION:Verify that the pmcstat erratum #1238 patch (SWLSVROS-6414 / D4) is applied to the binary located by amd.pmcstat.path or found in PATH; check the CPUID range logic covers Family 19h models 00h–0Fh.\n'
        ;;

    # ── UMCDF ─────────────────────────────────────────────────────────────
    umcdf_df_test:umcdf_df_runtime_smoke)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates an AMD Data Fabric PMC event, starts and stops sampling, then generates memory traffic to verify the counter increments correctly.\n'
        printf 'CAUSE:Either the FreeBSD PMU JSON tables in this tree do not carry DF event definitions for this CPU generation, or the test is disabled because amd.umcdf.df_runtime is not set to true in the kyua config.\n'
        printf 'ACTION:Add DF JSON tables for Zen4 to FreeBSD, or enable the test via kyua config: set amd.umcdf.df_runtime=true.\n'
        ;;

    umcdf_umc_test:umcdf_umc_runtime_smoke_if_supported)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Allocates an AMD UMC (Unified Memory Controller) PMC event, starts and stops sampling, and generates memory traffic to confirm the counter increments.\n'
        printf 'CAUSE:pmc_allocate() returned Operation not supported (EOPNOTSUPP) — the UMC PMC backend is not yet implemented in this libpmc/hwpmc tree even though UMC metadata is present.\n'
        printf 'ACTION:Implement the UMC PMC runtime backend in hwpmc and libpmc for Zen4 (EPYC 9xxx series).\n'
        ;;

    # ── hwpmc_exterr — extended error field not wired in kernel ───────────
    # All 14 TCs in hwpmc_exterr_test skip with "kernel hwpmc does not
    # populate extended errors yet".  One entry covers them all.
    hwpmc_exterr_test:pmcallocate_invalid_mode|\
    hwpmc_exterr_test:pmcallocate_invalid_cpu|\
    hwpmc_exterr_test:pmcallocate_invalid_flags|\
    hwpmc_exterr_test:pmcattach_system_mode|\
    hwpmc_exterr_test:pmcattach_running_pmc|\
    hwpmc_exterr_test:pmcrw_no_flags|\
    hwpmc_exterr_test:pmcrw_write_running|\
    hwpmc_exterr_test:amd_missing_pmu_flag|\
    hwpmc_exterr_test:amd_invalid_subclass|\
    hwpmc_exterr_test:amd_invalid_config_bits|\
    hwpmc_exterr_test:ibs_missing_system_capability|\
    hwpmc_exterr_test:ibs_invalid_type|\
    hwpmc_exterr_test:ibs_invalid_config_bits|\
    hwpmc_exterr_test:ibs_nonsampling_mode)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Exercises a specific PMC_OP_* ioctl error path (bad mode, bad CPU, bad flags, attach constraints, PMCRW constraints, or IBS-specific rejections) and asserts the kernel populates the extended-error sysctl with a human-readable reason string via pmc_save_uexterr().\n'
        printf 'CAUSE:require_working_exterr() skips the test because the kernel does not call pmc_save_uexterr() on any error path yet — the extended-error sysctl is always empty, so the assertion cannot be satisfied.\n'
        printf 'ACTION:Wire pmc_save_uexterr() calls into the relevant PMC_OP_* kernel handlers (hwpmc_ioctl in sys/dev/hwpmc/hwpmc.c) and re-run.\n'
        ;;

    # ── pmcstat_tsc_test — QA-branch -j flag not in installed pmcstat ─────
    pmcstat_tsc_test:json_tsc_unsigned)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Captures a live pmclog via pmcstat, then runs pmcstat -R -j to decode it as JSON and asserts every "tsc" field is a non-negative unsigned integer — verifying the %%jd-to-%%ju fix (SWLSVROS-6363) is present.\n'
        printf 'CAUSE:pmcstat -R test.pmc -j printed "illegal option" — the -j (JSON output) flag is not recognised by the pmcstat binary installed in /usr/sbin/pmcstat on this system; the QA branch carrying SWLSVROS-6363 has not been installed.\n'
        printf 'ACTION:Install the QA-branch pmcstat (SWLSVROS-6363 patch) to /usr/sbin/pmcstat or point amd.pmcstat.path at the correct binary, then re-run.\n'
        ;;

    # ── pmcstat_grouping_test — AMD core-grouping runtime disabled ─────────
    pmcstat_grouping_test:repeated_process_cycles_have_bounded_skew|\
    pmcstat_grouping_test:mixed_cycles_instructions_are_independent_columns|\
    pmcstat_grouping_test:mixed_cache_cycles_are_independent_columns|\
    pmcstat_grouping_test:oversubscribed_process_cycles_fail_cleanly)
        printf 'CLASS:normal\n'
        printf 'OPERATION:Runs a live pmcstat process-counting session with AMD Zen core events (ls_not_halted_cyc, ex_ret_instr, cache events) and checks that the grouping patch produces the correct column structure in the output.\n'
        printf 'CAUSE:The test guards behind amd.umcdf_skip_unless_pmu_grouping_runtime() which checks the kyua config key amd.pmc.grouping.runtime; it is not set to true in this environment, so the test self-skips. This is expected — live process-counting PMC tests are disabled by default.\n'
        printf 'ACTION:To enable: add "amd.pmc.grouping.runtime=true" to /usr/tests/sys/amd/pmc/kyua.conf (or pass it via kyua -v) and re-run.\n'
        ;;

    # ── Fallback ──────────────────────────────────────────────────────────
    *)
        printf 'CLASS:actionable\n'
        printf 'OPERATION:Unknown — no diagnostic entry for this test.\n'
        printf 'CAUSE:See skip/fail message above.\n'
        printf 'ACTION:Add a diag_for_test() entry in run.sh for this test ID.\n'
        ;;
    esac
}

# ── Category / filtered Kyuafile ───────────────────────────────────────────

# Return true (0) if test binary $1 belongs to any category in $2 (space-sep list)
test_in_categories() {
    _bin="$1"; _cats="$2"
    _meta=$(get_test_meta "$_bin")
    _bincat=$(printf '%s' "$_meta" | cut -d: -f1)
    for _c in $_cats; do
        [ "$_bincat" = "$_c" ] && return 0
    done
    return 1
}

# Print categories from $1 that are also present in $2.
category_intersection() {
    _left="$1"
    _right="$2"
    _out=""

    for _lc in $_left; do
        for _rc in $_right; do
            if [ "$_lc" = "$_rc" ]; then
                _out="$_out $_lc"
                break
            fi
        done
    done
    printf '%s' "$_out"
}

# Build a filtered Kyuafile containing only tests from the selected categories.
# The filtered file is written inside $1 (the install dir) so kyua can resolve
# test program names as relative paths from that directory.
# Prints the path of the file to use (original or filtered).
# If CATEGORIES is empty, prints the original Kyuafile path unchanged.
build_filtered_kyuafile() {
    _install_dir="$1"
    _original="${_install_dir}/Kyuafile"

    if [ -z "$CATEGORIES" ]; then
        printf '%s' "$_original"
        return 0
    fi

    # Write alongside the real Kyuafile so relative test-program names resolve.
    _tmp="${_install_dir}/.kyuafile_filtered_$$.tmp"
    # Copy header lines (syntax / test_suite declarations)
    grep -v '^atf_test_program' "$_original" > "$_tmp" 2>/dev/null || true

    # Include only tests whose category matches
    while IFS= read -r line; do
        case "$line" in
            atf_test_program*)
                _name=$(printf '%s' "$line" | sed 's/.*name="\([^"]*\)".*/\1/')
                if test_in_categories "$_name" "$CATEGORIES"; then
                    printf '%s\n' "$line" >> "$_tmp"
                fi
                ;;
        esac
    done < "$_original"

    _count=$(grep -c '^atf_test_program' "$_tmp" 2>/dev/null || echo 0)
    log_verbose "Filtered Kyuafile: $CATEGORIES → ${_count} test(s)"
    printf '%s' "$_tmp"
}

# ── Email reporting ────────────────────────────────────────────────────────

# mail_all <subject> <body> [addr_list]
# Sends body to every comma-separated address in addr_list (default: $REPORT_EMAIL).
mail_all() {
    _ma_subject="$1"
    _ma_body="$2"
    _ma_list="${3:-$REPORT_EMAIL}"
    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would email '${_ma_subject}' to '${_ma_list}' (dry run)"
        return 0
    fi
    _ma_IFS="$IFS"; IFS=','
    for _ma_addr in $_ma_list; do
        _ma_addr=$(printf '%s' "$_ma_addr" | tr -d ' ')
        [ -n "$_ma_addr" ] && printf 'From: %s\nTo: %s\nSubject: %s\n\n%s\n' \
                "$SENDER_EMAIL" "$_ma_addr" "$_ma_subject" "$_ma_body" \
                | sendmail -f "$SENDER_EMAIL" "$_ma_addr"
    done
    IFS="$_ma_IFS"
    log_success "Email sent to: ${_ma_list}"
}

# send_mime_report <to> <subject> <summary> <report_txt> <report_xml>
# Sends a MIME multipart/mixed email: plain-text summary as body,
# report.txt and report.xml as attachments (skipped if files absent).
send_mime_report() {
    _smr_to="$1"
    _smr_subject="$2"
    _smr_summary="$3"
    _smr_txt="$4"
    _smr_xml="$5"
    _smr_boundary="----=_AmdCIPart_$(date +%s)_$$"
    _smr_sep="--${_smr_boundary}"
    {
        printf 'From: %s\n' "$SENDER_EMAIL"
        printf 'To: %s\n' "$_smr_to"
        printf 'Subject: %s\n' "$_smr_subject"
        printf 'MIME-Version: 1.0\n'
        printf 'Content-Type: multipart/mixed; boundary="%s"\n' "$_smr_boundary"
        printf '\n'
        printf '%s\n' "$_smr_sep"
        printf 'Content-Type: text/plain; charset=utf-8\n'
        printf '\n'
        printf '%s\n' "$_smr_summary"
        printf '\n'
        if [ -f "$_smr_txt" ]; then
            printf '%s\n' "$_smr_sep"
            printf 'Content-Type: text/plain; name="report.txt"\n'
            printf 'Content-Disposition: attachment; filename="report.txt"\n'
            printf '\n'
            cat "$_smr_txt"
            printf '\n'
        fi
        if [ -f "$_smr_xml" ]; then
            printf '%s\n' "$_smr_sep"
            printf 'Content-Type: application/xml; name="report.xml"\n'
            printf 'Content-Disposition: attachment; filename="report.xml"\n'
            printf '\n'
            cat "$_smr_xml"
            printf '\n'
        fi
        printf '%s--\n' "$_smr_sep"
    } | sendmail -f "$SENDER_EMAIL" "$_smr_to"
}

# send_report_email <report_txt> <verdict> <email_addr>
send_report_email() {
    _report="$1"
    _verdict="$2"
    _to="${3:-$REPORT_EMAIL}"

    _subject="[AMD CI] ${SUITE} Test Suite: ${_verdict} - $(hostname -s) $(date +%Y-%m-%d)"

    if [ ! -f "$_report" ]; then
        log_warning "Report file not found: $_report -sending summary only"
        _body=$(printf 'AMD PMU CI Report\nVerdict: %s\nDate: %s\nHost: %s\n' \
            "$_verdict" "$(date)" "$(uname -n)")
        mail_all "$_subject" "$_body" "$_to"
        return
    fi

    _xml="${_report%.txt}.xml"
    _summary=$(
        printf 'AMD PMU CI -%s Test Suite Report\n' "$SUITE"
        printf 'Verdict  : %s\n' "$_verdict"
        printf 'Date     : %s\n' "$(date)"
        printf 'Host     : %s  (%s)\n' "$(uname -n)" "$(uname -r)"
        printf 'Suite    : %s\n' "$SUITE"
        [ -n "$CATEGORIES" ] && printf 'Categories: %s\n' "$CATEGORIES"
    )
    _sre_IFS="$IFS"; IFS=','
    for _sre_addr in $_to; do
        _sre_addr=$(printf '%s' "$_sre_addr" | tr -d ' ')
        [ -n "$_sre_addr" ] && send_mime_report "$_sre_addr" "$_subject" "$_summary" "$_report" "$_xml"
    done
    IFS="$_sre_IFS"
    log_success "Email sent to: ${_to}"
}

# ── Kernel build ───────────────────────────────────────────────────────────

# Build the kernel from $SRC_DIR using KERNCONF
build_kernel_from_src() {
    _ncpu=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
    log_info "Building kernel: KERNCONF=${AUTO_KERNCONF}  jobs=${_ncpu}"

    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would run: make -j${_ncpu} buildkernel KERNCONF=${AUTO_KERNCONF} (dry run)"
        return 0
    fi

    if [ ! -d "$SRC_DIR" ]; then
        log_error "Source directory not found: $SRC_DIR"
        log_error "Run --download first to clone the FreeBSD source."
        exit 1
    fi

    confirm_cmd "Build kernel KERNCONF=${AUTO_KERNCONF} in ${SRC_DIR} (${_ncpu} jobs, ~5–10 min)" \
        "make -C ${SRC_DIR} -j${_ncpu} buildkernel KERNCONF=${AUTO_KERNCONF}" || return 1

    if make -C "$SRC_DIR" -j"$_ncpu" buildkernel KERNCONF="$AUTO_KERNCONF"; then
        log_success "Kernel build complete"
    else
        log_error "Kernel build failed"
        exit 1
    fi
}

# Install the built kernel; old kernel preserved automatically as /boot/kernel.old
install_kernel_to_boot() {
    log_info "Installing kernel to /boot/kernel (old → /boot/kernel.old)..."

    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would run: make installkernel KERNCONF=${AUTO_KERNCONF} (dry run)"
        return 0
    fi

    confirm_cmd "Install kernel (replaces /boot/kernel; old saved as /boot/kernel.old)" \
        "make -C ${SRC_DIR} installkernel KERNCONF=${AUTO_KERNCONF}" || return 1

    if make -C "$SRC_DIR" installkernel KERNCONF="$AUTO_KERNCONF"; then
        log_success "Kernel installed to /boot/kernel"
        # nextboot -k kernel marks this kernel for the next boot explicitly
        nextboot -f -k kernel && log_verbose "nextboot set to kernel" || \
            log_warning "nextboot failed -kernel will still boot (it is the default)"
    else
        log_error "Kernel install failed"
        exit 1
    fi
}

# ── Auto-test sentinel and rc.d service ───────────────────────────────────

# Print a POSIX shell single-quoted representation of $1.
shell_quote() {
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}

# Write NAME=VALUE to stdout using shell_quote() for VALUE.
write_sentinel_var() {
    printf '%s=' "$1"
    shell_quote "$2"
    printf '\n'
}

valid_category_token() {
    case "$1" in
        TC-?*) ;;
        *) return 1 ;;
    esac
    case "$1" in
        *[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-]*) return 1 ;;
    esac
    return 0
}

# Write the sentinel file that the rc.d service reads after reboot
write_autotest_sentinel() {
    _email="${1:-$REPORT_EMAIL}"

    log_info "Writing autotest sentinel: ${AUTOTEST_SENTINEL}"
    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would write sentinel: ${AUTOTEST_SENTINEL} (dry run)"
        return 0
    fi
    _sentinel_commit=$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || echo "unknown")
    _trigger_time=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    {
        printf '%s\n' '# ibs-autotest sentinel -written by run.sh --auto'
        printf '# Consumed and deleted by %s after tests complete.\n' "$RCD_SERVICE"
        write_sentinel_var AUTOTEST_EMAIL "$_email"
        write_sentinel_var AUTOTEST_SUITE "$SUITE"
        write_sentinel_var AUTOTEST_SUITE_LIST "$SUITE_LIST"
        write_sentinel_var AUTOTEST_CATEGORIES "$CATEGORIES"
        write_sentinel_var AUTOTEST_SCRIPT_DIR "$SCRIPT_DIR"
        write_sentinel_var AUTOTEST_KERNCONF "$AUTO_KERNCONF"
        write_sentinel_var AUTOTEST_TRIGGER_TIME "$_trigger_time"
        write_sentinel_var AUTOTEST_SRC_COMMIT "$_sentinel_commit"
        write_sentinel_var AUTOTEST_WITH_STRESS "$WITH_STRESS"
    } > "$AUTOTEST_SENTINEL"
    chmod 600 "$AUTOTEST_SENTINEL"
    log_success "Sentinel written"
}

# Install the rc.d service that runs tests once after reboot
install_rcd_service() {
    log_info "Installing rc.d service: ${RCD_SERVICE}"
    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would install rc.d service: ${RCD_SERVICE} (dry run)"
        log_info "Would enable: sysrc ibs_autotest_enable=YES (dry run)"
        return 0
    fi

    cat > "$RCD_SERVICE" << 'RCEOF'
#!/bin/sh
#
# PROVIDE: ibs_autotest
# REQUIRE: NETWORKING LOGIN cleanvar
# KEYWORD: nojail
#
# ibs_autotest rc.d service -runs AMD PMU test suite once after --auto reboot.
# Self-disables after completion.  Written by run.sh --auto.

. /etc/rc.subr

name="ibs_autotest"
rcvar="${name}_enable"
start_cmd="${name}_run"
stop_cmd=":"

ibs_autotest_run()
{
    SENTINEL="/var/db/ibs-autotest-sentinel"
    LOG="/var/log/ibs-autotest.log"

    [ -f "$SENTINEL" ] || return 0

    # The sentinel is shell syntax.  Verify the file and parent directory before
    # dot-sourcing so rc.d never executes a replaced or writable file at boot.
    _sentinel_parent=${SENTINEL%/*}
    _sentinel_ok=$(find "$SENTINEL" -prune -type f -user root \
        ! -perm -020 ! -perm -002 2>/dev/null)
    if [ "$_sentinel_ok" != "$SENTINEL" ]; then
        echo "Invalid sentinel ownership, type, or mode: $SENTINEL" >> "$LOG"
        return 1
    fi
    _parent_ok=$(find "$_sentinel_parent" -prune -type d -user root \
        ! -perm -020 ! -perm -002 2>/dev/null)
    if [ "$_parent_ok" != "$_sentinel_parent" ]; then
        echo "Invalid sentinel parent ownership or mode: $_sentinel_parent" >> "$LOG"
        return 1
    fi

    # Read config from sentinel after the trust boundary above.
    . "$SENTINEL"
    SCRIPT="${AUTOTEST_SCRIPT_DIR}/run.sh"

    {
        echo "=== ibs_autotest rc.d started: $(date) ==="
        echo "Suite      : ${AUTOTEST_SUITE}"
        echo "Categories : ${AUTOTEST_CATEGORIES}"
        echo "Email      : ${AUTOTEST_EMAIL}"
        echo "Kernel     : ${AUTOTEST_KERNCONF}"
        echo ""
    } >> "$LOG" 2>&1

    # Disable ourselves so we don't run on the next reboot
    sysrc -x "${name}_enable" >> "$LOG" 2>&1 || sysrc "${name}_enable"=NO >> "$LOG" 2>&1

    # Remove sentinel to prevent re-run
    rm -f "$SENTINEL"

    # Validate sourced sentinel values before using them as paths or argv.
    case "$AUTOTEST_SUITE" in
        ""|DEFAULT|IBS|UMCDF|PMC|TSC|L3|STRESS|ALL) ;;
        *) echo "Invalid AUTOTEST_SUITE in sentinel: $AUTOTEST_SUITE" >> "$LOG"; return 1 ;;
    esac
    case "$AUTOTEST_WITH_STRESS" in
        ""|0|1) ;;
        *) echo "Invalid AUTOTEST_WITH_STRESS in sentinel: $AUTOTEST_WITH_STRESS" >> "$LOG"; return 1 ;;
    esac
    for _c in $AUTOTEST_CATEGORIES; do
        case "$_c" in
            TC-?*) ;;
            *) echo "Invalid AUTOTEST_CATEGORIES token in sentinel: $_c" >> "$LOG"; return 1 ;;
        esac
        case "$_c" in
            *[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-]*)
                echo "Invalid AUTOTEST_CATEGORIES token in sentinel: $_c" >> "$LOG"
                return 1
                ;;
        esac
    done

    # Build argv with set --.  Do not concatenate shell argument strings here.
    set -- --run-all --force
    [ -n "$AUTOTEST_SUITE" ]          && set -- "$@" --suite "$AUTOTEST_SUITE"
    [ "$AUTOTEST_WITH_STRESS" = "1" ] && set -- "$@" --stress
    for _c in $AUTOTEST_CATEGORIES; do
        set -- "$@" --category "$_c"
    done

    # Compile selected suites if their installed Kyuafile is missing.
    _autotest_suites="$AUTOTEST_SUITE_LIST"
    [ -z "$_autotest_suites" ] && _autotest_suites="$AUTOTEST_SUITE"
    case "$_autotest_suites" in
        ""|DEFAULT) _autotest_suites="IBS UMCDF PMC" ;;
        ALL)        _autotest_suites="IBS UMCDF PMC TSC L3 STRESS" ;;
    esac
    for _suite in $_autotest_suites; do
        case "$_suite" in
            IBS|UMCDF|PMC|TSC|L3|STRESS) ;;
            *) echo "Invalid suite token in sentinel: $_suite" >> "$LOG"; return 1 ;;
        esac
        _suite_lc=$(printf '%s' "$_suite" | tr '[:upper:]' '[:lower:]')
        if [ ! -f "/usr/tests/sys/amd/${_suite_lc}/Kyuafile" ]; then
            echo "=== Compiling $_suite test suite ===" >> "$LOG"
            if ! sh "$SCRIPT" --compile --suite "$_suite" --force >> "$LOG" 2>&1; then
                echo "=== Failed to compile $_suite test suite; aborting ===" >> "$LOG"
                return 1
            fi
        fi
    done

    # Run tests and capture report
    RESULTS_DIR="/var/log/ibs-autotest-results-$(date +%Y%m%d-%H%M%S)"
    echo "=== Running tests (results: $RESULTS_DIR) ===" >> "$LOG"
    sh "$SCRIPT" "$@" \
        --results-dir "$RESULTS_DIR" \
        >> "$LOG" 2>&1
    _rc=$?

    # Determine verdict from report
    _verdict="UNKNOWN"
    if [ -f "${RESULTS_DIR}/report.txt" ]; then
        if grep -q "VERDICT: APPROVED" "${RESULTS_DIR}/report.txt"; then
            _verdict="APPROVED"
        elif grep -q "VERDICT: CONDITIONAL" "${RESULTS_DIR}/report.txt"; then
            _verdict="CONDITIONAL"
        elif grep -q "VERDICT: NOT APPROVED" "${RESULTS_DIR}/report.txt"; then
            _verdict="NOT APPROVED"
        fi
    fi

    echo "=== Test run finished (rc=$_rc) verdict=$_verdict ===" >> "$LOG"

    # Email the report -MIME multipart: summary body + report.txt + report.xml attached
    _sender="freebsd-ci-actions@amd.com"
    _report="${RESULTS_DIR}/report.txt"
    _xml="${RESULTS_DIR}/report.xml"
    _subject="[AMD CI] ${AUTOTEST_SUITE} Tests: ${_verdict} - $(hostname -s) $(date +%Y-%m-%d)"
    _summary=$(
        printf 'AMD PMU CI -%s Test Suite Report\n' "$AUTOTEST_SUITE"
        printf 'Verdict  : %s\n' "$_verdict"
        printf 'Date     : %s\n' "$(date)"
        printf 'Host     : %s  (%s)\n' "$(uname -n)" "$(uname -r)"
        printf 'Kernel   : %s\n' "$AUTOTEST_KERNCONF"
        printf 'Trigger  : %s\n' "$AUTOTEST_TRIGGER_TIME"
    )
    _boundary="----=_AmdCIPart_$(date +%s)_$$"
    _sep="--${_boundary}"
    _rcd_IFS="$IFS"; IFS=','
    for _rcd_addr in $AUTOTEST_EMAIL; do
        _rcd_addr=$(printf '%s' "$_rcd_addr" | tr -d ' ')
        [ -z "$_rcd_addr" ] && continue
        {
            printf 'From: %s\n' "$_sender"
            printf 'To: %s\n' "$_rcd_addr"
            printf 'Subject: %s\n' "$_subject"
            printf 'MIME-Version: 1.0\n'
            printf 'Content-Type: multipart/mixed; boundary="%s"\n' "$_boundary"
            printf '\n'
            printf '%s\n' "$_sep"
            printf 'Content-Type: text/plain; charset=utf-8\n'
            printf '\n'
            printf '%s\n' "$_summary"
            printf '\n'
            if [ -f "$_report" ]; then
                printf '%s\n' "$_sep"
                printf 'Content-Type: text/plain; name="report.txt"\n'
                printf 'Content-Disposition: attachment; filename="report.txt"\n'
                printf '\n'
                cat "$_report"
                printf '\n'
            fi
            if [ -f "$_xml" ]; then
                printf '%s\n' "$_sep"
                printf 'Content-Type: application/xml; name="report.xml"\n'
                printf 'Content-Disposition: attachment; filename="report.xml"\n'
                printf '\n'
                cat "$_xml"
                printf '\n'
            fi
            printf '%s--\n' "$_sep"
        } | sendmail -f "$_sender" "$_rcd_addr"
    done
    IFS="$_rcd_IFS"

    echo "=== Report emailed to $AUTOTEST_EMAIL ===" >> "$LOG"

    # Record the tested source commit so the next --auto run can skip if unchanged
    if [ -n "$AUTOTEST_SRC_COMMIT" ] && [ "$AUTOTEST_SRC_COMMIT" != "unknown" ]; then
        printf '%s\n' "$AUTOTEST_SRC_COMMIT" > /var/db/ibs-autotest-last-commit
        echo "=== Recorded last-tested commit: $AUTOTEST_SRC_COMMIT ===" >> "$LOG"
    fi
}

load_rc_config $name
run_rc_command "$1"
RCEOF

    chmod 555 "$RCD_SERVICE"
    log_success "rc.d service installed: ${RCD_SERVICE}"

    # Enable it for the next boot only; it self-disables after running.
    # Use the literal service name -${name} is only defined inside the rc.d script.
    sysrc ibs_autotest_enable=YES || { log_error "sysrc failed — ibs_autotest will NOT run after reboot"; return 1; }
    log_success "ibs_autotest enabled for next boot (self-disables after run)"
}

# ── --auto orchestration ───────────────────────────────────────────────────

auto_mode() {
    _email="${1:-$REPORT_EMAIL}"

    # Step 0: Fetch latest from GitHub so the comparison reflects remote HEAD,
    # not whatever happens to be in the local working tree.
    if [ -d "$SRC_DIR/.git" ]; then
        # Ensure origin points to the configured REPO_URL (may have changed)
        _cur_remote=$(git -C "$SRC_DIR" remote get-url origin 2>/dev/null || echo "")
        if [ "$_cur_remote" != "$REPO_URL" ]; then
            log_info "Updating origin remote: ${_cur_remote} → ${REPO_URL}"
            git -C "$SRC_DIR" remote set-url origin "$REPO_URL" 2>/dev/null || true
        fi

        log_info "Fetching latest from ${REPO_URL} branch ${BRANCH}..."
        if [ $DRY_RUN -eq 0 ]; then
            git -C "$SRC_DIR" fetch origin "$BRANCH" 2>/dev/null || \
                log_warning "git fetch failed -falling back to local HEAD for commit check"
        else
            log_info "Would run: git fetch origin ${BRANCH} (dry run)"
        fi
    fi

    # Resolve the remote HEAD using the unambiguous full ref path with --verify
    # (branch names with slashes confuse plain rev-parse and produce stdout noise)
    _current_commit=$(git -C "$SRC_DIR" rev-parse --verify \
                          "refs/remotes/origin/${BRANCH}" 2>/dev/null || \
                      git -C "$SRC_DIR" rev-parse --verify HEAD 2>/dev/null || echo "")

    # Check if this commit was already built and tested
    if [ $FORCE_REBUILD -eq 1 ]; then
        log_info "Skipping commit check (--force-rebuild): will build regardless of prior run"
    elif [ -n "$_current_commit" ] && [ -f "$LAST_COMMIT_FILE" ]; then
        _last_commit=$(cat "$LAST_COMMIT_FILE")
        if [ "$_current_commit" = "$_last_commit" ]; then
            log_info "Kernel is up to date -commit ${_current_commit} already tested"
            _uptodate_body=$(
                printf 'AMD PMU CI -Kernel is up to date and already tested\n'
                printf '\n'
                printf 'No new commits on branch %s since the last test run.\n' "$BRANCH"
                printf '\n'
                printf 'Host     : %s  (%s)\n' "$(uname -n)" "$(uname -r)"
                printf 'Suite    : %s\n' "$SUITE"
                printf 'Kernel   : %s\n' "$AUTO_KERNCONF"
                printf 'Commit   : %s\n' "$_current_commit"
                printf 'Repo     : %s\n' "$REPO_URL"
                printf 'Date     : %s\n' "$(date)"
                printf '\n'
                printf 'Nothing to do -no build or reboot was started.\n'
            )
            mail_all "[AMD CI] Kernel up to date, already tested - $(hostname -s) $(date +%Y-%m-%d)" \
                "$_uptodate_body" "$_email"
            generate_html_skipped_report "$_current_commit" "$BRANCH"
            return 0
        fi
    fi

    log_info "New commit detected: ${_current_commit}"
    log_info "Auto mode: fetch → reset → build kernel → install → write sentinel → reboot"
    log_info "Suite: ${SUITE}  Kernel: ${AUTO_KERNCONF}  Email: ${_email}"

    check_boot_environment
    check_root_privileges

    # Step 1: Update local source tree to the fetched remote HEAD before building
    if [ -d "$SRC_DIR/.git" ] && [ $DRY_RUN -eq 0 ]; then
        log_info "Resetting ${SRC_DIR} to origin/${BRANCH}..."
        git -C "$SRC_DIR" reset --hard "origin/${BRANCH}" || {
            log_error "Failed to reset source tree to origin/${BRANCH}"
            exit 1
        }
        git -C "$SRC_DIR" clean -fd > /dev/null 2>&1 || true
    fi

    # Step 2: Build kernel
    build_kernel_from_src

    # Step 3: Install kernel (marks /boot/kernel for next boot)
    install_kernel_to_boot

    # Step 4: Compile test suite (so rc.d service doesn't need to)
    log_info "Pre-compiling test suite..."
    compile_tests

    # Step 5: Write sentinel
    write_autotest_sentinel "$_email"

    # Step 6: Install + enable rc.d service
    install_rcd_service

    # Step 7: Reboot
    echo ""
    echo "================================================================="
    log_info "System is ready for auto-test reboot."
    log_info "After reboot:"
    log_info "  1. rc.d/ibs_autotest will run the ${SUITE} test suite"
    log_info "  2. Report will be emailed to: ${_email}"
    log_info "  3. Log at: /var/log/ibs-autotest.log"
    log_info "  4. Service will self-disable (runs exactly once)"
    log_info "  Fallback kernel: /boot/kernel.old"
    echo "================================================================="
    echo ""

    confirm_cmd "Reboot now into the new kernel for automated test run" \
        "reboot" || { log_info "Reboot cancelled. Run 'reboot' manually when ready."; return 0; }

    [ $DRY_RUN -eq 0 ] && reboot || log_info "Dry run -would reboot here"
}

# ── --last-test: show what ran last and what may have caused a panic ───────

show_last_test() {
    echo ""
    echo "================================================================="
    echo "LAST TEST RUN — PANIC RECOVERY REPORT"
    echo "================================================================="

    # 1. Run state metadata
    if [ -f "$LAST_TEST_STATE" ]; then
        echo ""
        echo "Last run state:"
        cat "$LAST_TEST_STATE"
    else
        printf '  No state file found (%s)\n' "$LAST_TEST_STATE"
        printf '  Run --run-all at least once to populate it.\n'
    fi

    # 2. Most recent results directory
    echo ""
    _latest_results=""
    if [ -d "$HTML_DIR" ]; then
        _latest_results=$(ls -dt "$HTML_DIR/results-"* 2>/dev/null | head -1)
    fi
    if [ -n "$_latest_results" ]; then
        printf 'Most recent results dir: %s\n' "$_latest_results"
        [ -f "${_latest_results}/report.txt" ] && \
            printf '  report.txt : %s/report.txt\n' "$_latest_results"
        [ -f "${_latest_results}/report.xml" ] && \
            printf '  report.xml : %s/report.xml\n' "$_latest_results"
    else
        printf '  No results directory found under %s\n' "$HTML_DIR"
    fi

    # 3. Live run log — last 60 lines (most useful for panic triage)
    echo ""
    if [ -f "$LAST_RUN_LOG" ]; then
        _log_lines=$(wc -l < "$LAST_RUN_LOG" 2>/dev/null || echo 0)
        printf 'Live run log (%s) — last 60 of %d lines:\n' "$LAST_RUN_LOG" "$_log_lines"
        echo "-----------------------------------------------------------------"
        tail -60 "$LAST_RUN_LOG"
        echo "-----------------------------------------------------------------"
        echo ""
        echo "TIP: If the system panicked mid-run, the test AFTER the last"
        echo "     completed entry above is the likely culprit."
        echo "     Cross-reference with: dmesg | tail -100"
        echo "     and: /var/log/messages"
    else
        printf '  No live run log found (%s)\n' "$LAST_RUN_LOG"
        printf '  The log is created on the first --run-all invocation.\n'
    fi

    echo "================================================================="
}

# Show usage information
show_usage() {
    cat << EOF
${CYAN}════════════════════════════════════════════════════════════════${NC}
${WHITE}  IBS Test Suite Manager  v${SCRIPT_VERSION}${NC}
${CYAN}════════════════════════════════════════════════════════════════${NC}

${BOLD}DESCRIPTION${NC}
    This script manages the full lifecycle of AMD hardware performance
    monitoring (PMU) tests on FreeBSD bare-metal systems.  It covers
    three test suites:

      IBS   -Instruction-Based Sampling (AMD-exclusive hardware feature)
      UMCDF -Unified Memory Controller + Data Fabric PMU counters
      PMC   -General-purpose hardware performance counters (hwpmc)

    Tests are written in C using the ATF framework and run through kyua.
    Each test is tagged with a severity level that drives the overall
    pass/fail verdict:

      CRITICAL -any single failure is an immediate overall FAIL
      HIGH     -the suite FAIL if the pass rate drops below 95 %
      MEDIUM   -the suite is CONDITIONAL if the pass rate drops below 80 %

    The script can also build a custom FreeBSD kernel, schedule an
    automated post-reboot test run, and deliver the result by email.

${YELLOW}USAGE${NC}
    $0 [OPTIONS] [COMMAND]
    $0                           # no arguments → interactive menu

${YELLOW}COMMANDS${NC}

  ${BOLD}--download${NC}
      Clone or update the FreeBSD fork that contains the test source.
      Target: $REPO_URL  branch: $BRANCH
      Destination: $SRC_DIR
      On first run a fresh clone is performed.  On subsequent runs the
      existing tree is reset to origin/$BRANCH (uncommitted changes are
      discarded).  Requires root and an active Boot Environment as a
      safety net.

  ${BOLD}--compile${NC}
      Build and install the test suite selected by --suite (default selector
      uses IBS as the primary build suite).
      Runs 'make clean', 'make -j<ncpu>', and 'make install' inside the
      suite source directory (tests/sys/amd/<suite>/).  Binaries and the
      source-tree Kyuafile land in /usr/tests/sys/amd/<suite>/.
      Requires root.

  ${BOLD}--run-all${NC}
      Execute the installed test suite with kyua and show live output.
      Each result line is annotated with its [CATEGORY][SEVERITY] tag.
      After all tests finish, a severity-based verdict is printed:
        APPROVED      -all CRITICAL passed, HIGH ≥ 95 %, MEDIUM ≥ 80 %
        CONDITIONAL   -CRITICAL/HIGH passed but MEDIUM < 80 %
        NOT APPROVED  -any CRITICAL failed, or HIGH < 95 %
      A plain-text report and a JUnit XML report are saved to
      \$RESULTS_DIR (default: \$HTML_DIR/results-<timestamp>/).
      Combine with --category to run only a subset of tests.
      Requires root.

  ${BOLD}--run TEST${NC}
      Run a single named test binary (e.g. ibs_detect_test) through
      kyua and print a focused pass/fail report.  Useful for debugging
      a specific test case without running the full suite.  Requires
      root.

  ${BOLD}--list${NC}
      List every test binary installed under \$TESTS_INSTALL_DIR, with
      its category code, human-readable label, and severity level.
      Also prints placeholder test areas that are defined in the plan
      but not yet implemented, so the full roadmap is visible.

  ${BOLD}--report${NC}
      Re-display the last kyua run report in verbose mode (all result
      filters: passed, skipped, expected_failure, broken, failed).
      Also regenerates the JUnit XML file.  Does not run tests; reads
      the existing kyua results database.

  ${BOLD}--status${NC}
      Snapshot of the current system state:
        • CPU vendor and IBS CPUID/MSR feature-bit status
        • Whether the GitHub fork has been cloned and its HEAD commit
        • Whether tests are compiled and how many binaries are installed
        • Whether the cpuctl device is present and MSRs are accessible
        • Suggested next steps

  ${BOLD}--clean${NC}
      Remove build artifacts from the test source directory by running
      'make clean'.  The work/ results directory is left intact so
      previous reports are not lost; remove it manually if needed.

  ${BOLD}--loadmodule${NC}
      Load the cpuctl(4) kernel module (kldload cpuctl).  The cpuctl
      device (/dev/cpuctl<N>) is required by tests that read or write
      MSRs and CPUID leaves directly.  Equivalent to adding
      'cpuctl_load="YES"' to /boot/loader.conf for persistence.
      Requires root.

  ${BOLD}--fetch${NC}
      Pull the latest commits from origin/main of the freebsd-ci-actions
      repo itself (github.com/ojanerif/freebsd-ci-actions).  Shows the
      commits added since the previous HEAD.  Safe to run at any time;
      will not overwrite local uncommitted changes.

  ${BOLD}--push${NC}
      Push local commits on main to origin/main of the freebsd-ci-actions
      repo.  Checks how many commits are ahead of origin before pushing
      and asks for confirmation.  Does nothing if already up-to-date.

  ${BOLD}--commit${NC}
      Sync test sources and CI tooling to the AMD sos-git mirror and
      push.  Specifically:
        • Copies tests/sys/amd/ibs/ into \$SOS_DIR/tests/sys/amd/ibs/
        • Copies tests/sys/amd/pmc/ into \$SOS_DIR/tests/sys/amd/pmc/
        • Copies ci/tools/           into \$SOS_DIR/ci/tools/
        • Commits with message "amd: update tests and ci tools -<date>"
        • Pushes HEAD to ssh://git@sos-git.amd.com/freebsd-src.git
          branch $SOS_BRANCH
      \$SOS_DIR must already be a valid git clone of that remote.
      Asks for confirmation before committing and again before pushing.

  ${BOLD}--auto${NC}
      Fully-automated test cycle across a reboot.  Safe to run from cron —
      no interactive prompts are issued.  Steps in order:

        0. Check /var/db/ibs-autotest-last-commit against the current HEAD
           of the source tree.  If they match, the source has not changed
           since the last test run: a notification email is sent and the
           script exits without building or rebooting.
        1. Build the kernel: make -j<ncpu> buildkernel KERNCONF=<conf>
           (source must already be present; run --download first).
        2. Install the kernel: make installkernel; old kernel is saved
           to /boot/kernel.old automatically by installkernel.
        3. Mark the new kernel for next boot via nextboot(8).
        4. Pre-compile the test suite so the rc.d service runs tests
           immediately after boot without needing a build step.
        5. Write a sentinel file to /var/db/ibs-autotest-sentinel that
           records suite, categories, email, kernel config, and source
           commit hash (AUTOTEST_SRC_COMMIT).
        6. Install an rc.d service (/usr/local/etc/rc.d/ibs_autotest)
           that fires once after the next boot, runs the test suite,
           emails the report, then self-disables via sysrc.
        7. Reboot immediately (no confirmation prompt).

      After reboot the rc.d service runs unattended: it reads the
      sentinel, executes run.sh --run-all, determines the verdict,
      sends the full plain-text report to the configured email address
      via the system MTA (dma → txsmtp.amd.com), and writes the tested
      source commit to /var/db/ibs-autotest-last-commit so the next
      --auto invocation can skip an unchanged tree.
      Log: /var/log/ibs-autotest.log

      Boot Environment safety check is skipped in --auto mode (the
      operator is expected to have a safety net in place for unattended
      operation; use --force or manual invocation for a checked build).

  ${BOLD}--help${NC}
      Show this help message and exit.

${YELLOW}SUITE SELECTION${NC}
    --suite IBS         Instruction-Based Sampling tests
    --suite UMCDF       UMC + Data Fabric PMU tests
    --suite PMC         General hwpmc counter tests
    --suite TSC         TSC drift/invariance tests
    --suite L3          L3 PMU detection and hit/miss tests
    --suite STRESS      Standalone stress-test suite (cpu/mem/net/disk ATF tests)
    --suite ALL         All suites: IBS + UMCDF + PMC + TSC + L3 + STRESS
    (default)           IBS + UMCDF + PMC  (all production suites)

    Affects which source directory is compiled (--compile) and which
    install directory is used for test runs (--run-all, --list, etc.).

${YELLOW}CATEGORY SELECTION${NC}
    Combine one or more --category flags with --run-all to run only
    tests in those categories.  Internally a filtered Kyuafile is
    generated so kyua only sees the selected test binaries.
    --category can be repeated.

    IBS categories:
      TC-DET   Hardware Detection   Verify IBS feature bits in CPUID/MSR  [CRITICAL]
      TC-MSR   MSR Control         Read/write IBS control MSRs             [CRITICAL]
      TC-INT   Interrupt Delivery  IBS interrupt routing and delivery       [HIGH]
      TC-DATA  Sample Accuracy     Validate IBS sample data fields          [HIGH]
      TC-SMP   SMP / Per-CPU      Per-CPU IBS enable/disable on all cores  [HIGH]
      TC-HWPMC hwpmc API          hwpmc(4) alloc/caps/info/runtime paths   [HIGH]
      TC-DRV   Driver Access      cpuctl(4) ioctl and device-node access   [HIGH]
      TC-CONC  Concurrency        Concurrent IBS enable from multiple threads [HIGH]
      TC-SEC   Security           Access-control and invalid-input rejection [HIGH]
      TC-API   Userspace API      High-level userspace PMC API paths       [MEDIUM]
      TC-STR   Stress             CPU and memory stress under IBS sampling  [MEDIUM]
      TC-UNIT  Unit Tests         Pure-software unit tests; no hardware needed [MEDIUM]

    UMCDF categories:
      TC-UMCDET  UMC/DF Detection  CPUID-based UMC and DF feature detection [CRITICAL]
      TC-UMCPMC  UMC/DF PMC       UMC and DF counter read/write paths       [HIGH]
      TC-UMCUNIT UMCDF Unit       Software unit tests for decode/map logic  [MEDIUM]

    PMC categories:
      TC-PMCAPI   hwpmc API       General hwpmc API and error paths         [HIGH]
      TC-PMCSTAT  pmcstat Decode  pmcstat command and pmclog decode paths   [HIGH/MEDIUM]

    TSC categories:
      TC-TSC-DET  TSC Detection   TSC capability and frequency discovery     [CRITICAL]
      TC-TSC-DRF  TSC Drift       Cross-CPU TSC drift validation             [HIGH]
      TC-TSC-INV  TSC Invariant   Invariant-TSC behavior checks             [HIGH]
      TC-TSCSTR   TSC Stress      TSC behavior under sustained CPU load      [HIGH]

    L3 categories:
      TC-DET-L3   L3 Detection    L3 PMU availability and metadata checks    [CRITICAL]
      TC-UNC-L3   L3 PMC          L3 hit/miss event validation               [HIGH]

    STRESS categories:
      TC-CSTR  CPU Stress     Per-CPU compute, context-switch, FPU load     [MEDIUM]
      TC-MSTR  Memory Stress  Cache thrash, bandwidth, TLB pressure         [MEDIUM]
      TC-NSTR  Network Stress UNIX sockets, TCP loopback, connection rate   [MEDIUM]
      TC-DSTR  Disk Stress    Sequential, random, fsync I/O under /tmp      [MEDIUM]
      TC-ISTR  IBS-Op Stress    IBS Op sampling concurrent with each stressor   [MEDIUM]
      TC-FSTR  IBS-Fetch Stress IBS Fetch sampling concurrent with each stressor [MEDIUM]

${YELLOW}STRESS OPTIONS${NC}
    --stress            Launch cpu/mem/net/disk background stressor processes
                        while the primary test suite runs.  Lets you validate
                        that hardware counter operations behave correctly under
                        sustained system load.  Works with --run-all and --auto.
                        WARNING: combining --stress with IBS tests at high
                        sampling rates can reproduce the NMI storm crash in
                        FreeBSD-Tests-026.  Use --safe-ibs-rate to cap the
                        minimum IBS sampling period in that case.

    --safe-ibs-rate N   Set dev.hwpmc.ibs.min_period=N via sysctl before
                        starting the test run.  Caps the maximum IBS NMI rate
                        when using --stress + IBS suite together.  Has no
                        effect if the sysctl is not present in the kernel.

${YELLOW}PANIC RECOVERY${NC}
    --last-test         Show the last test run's live output and state.
                        Every --run-all writes a live log to:
                          ${LAST_RUN_LOG}
                        and a state file to:
                          ${LAST_TEST_STATE}
                        Both files survive a kernel panic and reboot.
                        After a panic, run --last-test to see which test was
                        the last to complete before the crash.  The test
                        immediately following it in Kyuafile order is the
                        likely culprit.  Cross-reference with dmesg.

${YELLOW}AUTO MODE OPTIONS${NC}
    --kernconf CONF     Kernel configuration to build with make buildkernel
                        (default: GENERIC).  Use AMD_IBS for the IBS-enabled
                        config if it is present in the FreeBSD source tree.
    --email ADDR        Email address for the post-reboot report (--auto always
                        emails).  For --run-all, email is sent only when --email
                        is explicitly given on the command line.
                        Default: freebsd-test@mailman-svr.amd.com,ojanerif@amd.com
                        Delivery uses the system MTA (dma → atlsmtp10.amd.com).
    --force-rebuild     Skip the "already tested" commit check.  Forces --auto
                        to proceed with build → reboot → test even when the
                        source tree has not changed since the last run.

${YELLOW}OPTIONS${NC}
    -v, --verbose       Print additional diagnostic messages (git SHAs,
                        kyua paths, filtered Kyuafile counts, etc.)
    -f, --force         Skip the Boot Environment safety check.  Use only
                        when you are certain a rollback path exists.
    -n, --dry-run       Print what each step would do without executing
                        anything.  No files are written, no commands run,
                        no modules loaded, no reboots triggered.
    --parallelism N     Number of parallel kyua workers (default: hw.ncpu).
                        Set to 1 to serialize test execution.
    --results-dir DIR   Directory where reports are saved after --run-all.
                        Default: \$HTML_DIR/results-<timestamp>
    -h, --help          Show this help and exit.

${YELLOW}EXAMPLES${NC}
    # Full first-time IBS workflow
    $0 --download --compile --run-all

    # Run IBS suite under background stress load and email the report
    $0 --run-all --stress --email me@amd.com

    # Run IBS + stress with a capped NMI rate (safer for NMI-storm scenarios)
    $0 --run-all --stress --safe-ibs-rate 0x1000

    # Run only unit and hwpmc-API tests (no hardware needed for unit)
    $0 --run-all --category TC-UNIT --category TC-HWPMC

    # Run IBS suite with concurrent background stress (validates IBS under load)
    $0 --suite IBS --run-all --stress --force

    # Run the standalone STRESS suite (validates stressors pass on their own)
    $0 --suite STRESS --compile --run-all

    # Run UMCDF suite
    $0 --suite UMCDF --compile --run-all

    # Debug a single failing test
    $0 --run ibs_detect_test

    # After a kernel panic: see which test was running at crash time
    $0 --last-test

    # Preview the auto workflow without touching anything
    $0 --dry-run --auto --suite IBS --kernconf GENERIC --email me@amd.com

    # Full automated kernel-build + reboot + test + email cycle (with stress)
    $0 --auto --suite IBS --kernconf GENERIC --stress --email me@amd.com

    # Sync changes to sos-git
    $0 --commit

    # Pull and push this repo's own CI actions
    $0 --fetch
    $0 --push

${YELLOW}VERDICT CRITERIA${NC}
    The overall verdict after --run-all is determined by severity thresholds
    defined in the AMD IBS Testing Plan:

      APPROVED      CRITICAL failures = 0, HIGH pass rate ≥ 95 %, MEDIUM ≥ 80 %
      CONDITIONAL   CRITICAL failures = 0, HIGH pass rate ≥ 95 %, MEDIUM < 80 %
      NOT APPROVED  Any CRITICAL test failed  —or—  HIGH pass rate < 95 %

    Skipped and expected-failure results are excluded from pass-rate
    calculations.

${YELLOW}FILES${NC}
    \$SCRIPT_DIR/tests/sys/amd/ibs/     IBS test source
    \$SCRIPT_DIR/tests/sys/amd/umcdf/   UMCDF test source
    \$SCRIPT_DIR/tests/sys/amd/stress/  STRESS test source + stressor binaries
    /usr/tests/sys/amd/ibs/            Installed IBS tests (kyua target)
    /usr/tests/sys/amd/umcdf/          Installed UMCDF tests
    /usr/tests/sys/amd/stress/         Installed stress tests + stressor binaries
    \$RESULTS_DIR/report.txt            Plain-text run report
    \$RESULTS_DIR/report.xml            JUnit XML (for CI systems)
    /var/log/ibs-last-run.log          Live kyua output (survives kernel panic)
    /var/log/ibs-last-test.state       Last completed test + metadata (panic triage)
    /var/db/ibs-autotest-sentinel      --auto sentinel (removed after run)
    /usr/local/etc/rc.d/ibs_autotest   --auto rc.d service (self-disables)
    /var/log/ibs-autotest.log          --auto post-reboot run log

${YELLOW}SOURCES${NC}
    GitHub fork : $REPO_URL  (branch: $BRANCH)
    sos-git     : ssh://git@sos-git.amd.com/freebsd-src.git  (branch: $SOS_BRANCH)
    HTML dir    : $HTML_DIR

${YELLOW}REQUIREMENTS${NC}
    - Root privileges (MSR access, module loading, test execution)
    - AMD CPU with IBS support (Family 10h or later)
    - FreeBSD 13+ with kyua installed (pkg install kyua)
    - cpuctl(4) module available (kldload cpuctl  or  --loadmodule)
    - For --auto: FreeBSD source tree at $SRC_DIR  (run --download)
    - For email delivery: system MTA configured (dma default on FreeBSD)

EOF
}

# Logging functions
log_info() {
    printf '%s\n' "${BLUE}[INFO]${NC} $1"
}

log_success() {
    printf '%s\n' "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    printf '%s\n' "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    printf '%s\n' "${RED}[ERROR]${NC} $1"
}

log_verbose() {
    if [ $VERBOSE -eq 1 ]; then
        printf '%s\n' "${PURPLE}[VERBOSE]${NC} $1"
    fi
}

# Confirm before executing an important command.
# Usage: confirm_cmd "short description" "full command string"
# Returns 0 to proceed, 1 if the user cancels.
# In AUTO_MODE or FORCE mode, always proceeds without prompting.
confirm_cmd() {
    _cc_desc="$1"
    _cc_cmd="$2"
    if [ $AUTO_MODE -eq 1 ] || [ $FORCE -eq 1 ]; then
        log_verbose "Auto/force mode: proceeding without confirmation: ${_cc_desc}"
        return 0
    fi
    printf '\n'
    printf '%s\n' "${CYAN}── Command ──────────────────────────────────────────────────────${NC}"
    [ -n "$_cc_desc" ] && printf '  %s%s%s\n' "$DIM" "$_cc_desc" "$NC"
    printf '  %s\$ %s%s\n' "$BOLD" "$_cc_cmd" "$NC"
    printf '%s\n' "${CYAN}─────────────────────────────────────────────────────────────────${NC}"
    printf 'Proceed? [y/N] '
    read -r _cc_yn
    case "$_cc_yn" in
        y|Y|yes|YES) return 0 ;;
        *) log_info "Cancelled."; return 1 ;;
    esac
}

# Safety checks
check_boot_environment() {
    if [ $AUTO_MODE -eq 1 ]; then
        log_verbose "Auto mode: skipping boot environment check"
        return 0
    fi
    if [ $FORCE -eq 1 ]; then
        log_warning "Skipping boot environment check (--force enabled)"
        return 0
    fi

    log_info "Checking Boot Environment safety..."

    TODAY=$(date +%Y%m%d)
    # Accept any backup BE, not just today's -a BE from yesterday is still a valid safety net
    BE_COUNT=$(bectl list 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')
    if [ "${BE_COUNT:-0}" -lt 2 ]; then
        log_error "No backup Boot Environment found (only active BE exists)"
        log_error "Create one first: sudo bectl create dev-backup-$TODAY"
        exit 1
    fi

    log_success "Boot Environment check passed"
}

check_root_privileges() {
    if [ $DRY_RUN -eq 1 ]; then
        log_verbose "Dry run: skipping root-privilege check"
        return 0
    fi
    if [ "$(id -u)" -ne 0 ]; then
        log_error "Root privileges required for IBS testing"
        log_error "Run with: sudo $0 $@"
        exit 1
    fi
}

preflight_checks() {
    log_info "Running preflight checks..."
    PREFLIGHT_OK=1

    # kyua must be installed
    if command -v kyua >/dev/null 2>&1; then
        log_verbose "kyua found: $(command -v kyua)"
    else
        log_error "kyua not found -install: pkg install kyua"
        PREFLIGHT_OK=0
    fi

    # cpuid utility (used by IBS tests to inspect CPU features)
    if command -v cpuid >/dev/null 2>&1; then
        log_verbose "cpuid found: $(command -v cpuid)"
    else
        log_warning "cpuid not found -some tests may be skipped (pkg install cpuid)"
    fi

    # cpuctl kernel module / device (required for MSR/CPUID access)
    if kldstat -q -n cpuctl 2>/dev/null; then
        log_verbose "cpuctl module loaded"
    elif [ -c /dev/cpuctl0 ]; then
        log_verbose "cpuctl device present (/dev/cpuctl0)"
    else
        log_error "cpuctl not available -load it: kldload cpuctl  (or add 'cpuctl_load=\"YES\"' to /boot/loader.conf)"
        PREFLIGHT_OK=0
    fi

    if [ "$PREFLIGHT_OK" -eq 0 ]; then
        log_error "Preflight failed -fix the errors above before running tests"
        exit 1
    fi

    log_success "Preflight checks passed"
}

# Repository management
sync_repository() {
    log_info "Syncing IBS test suite from repository..."

    if [ ! -d "$SRC_DIR/.git" ]; then
        log_verbose "Performing fresh clone..."
        if [ $DRY_RUN -eq 0 ]; then
            confirm_cmd "Clone $REPO_URL (branch: $BRANCH) into $SRC_DIR" \
                "git clone -b $BRANCH $REPO_URL $SRC_DIR" || return 1
            git clone -b "$BRANCH" "$REPO_URL" "$SRC_DIR" || {
                log_error "Failed to clone repository"
                exit 1
            }
        fi
    else
        log_verbose "Updating existing repository..."
        if [ $DRY_RUN -eq 0 ]; then
            confirm_cmd "Update $SRC_DIR -fetch, reset to origin/$BRANCH, clean untracked" \
                "git fetch origin $BRANCH && git reset --hard origin/$BRANCH && git clean -fd" || return 1
            cd "$SRC_DIR" || exit 1
            git fetch origin "$BRANCH" || {
                log_error "Failed to fetch from repository"
                exit 1
            }
            git reset --hard "origin/$BRANCH" || {
                log_error "Failed to reset repository"
                exit 1
            }
            git clean -fd || {
                log_error "Failed to clean repository"
                exit 1
            }
        fi
    fi

    if [ $DRY_RUN -eq 0 ]; then
        cd "$SRC_DIR" || exit 1
        LAST_COMMIT=$(git log -1 --format="%h %s (%cd)" --date=format:'%Y-%m-%d %H:%M')
        log_success "Repository synced - Last commit: $LAST_COMMIT"
    else
        log_info "Would sync repository (dry run)"
    fi
}

# Build and install tests
compile_tests() {
    _ct_suite="$SUITE"
    case "$_ct_suite" in
        DEFAULT|ALL) _ct_suite="$_SUITE_PRIMARY" ;;
    esac
    log_info "Building $_ct_suite test suite..."

    if [ ! -d "$TESTS_DIR" ]; then
        log_error "Test source directory not found: $TESTS_DIR"
        exit 1
    fi

    cd "$TESTS_DIR" || exit 1

    if [ $DRY_RUN -eq 0 ]; then
        _ncpu=$(sysctl -n hw.ncpu)

        log_verbose "Cleaning previous build..."
        confirm_cmd "Remove previous build artifacts in $TESTS_DIR" \
            "make clean" || return 1
        make clean || {
            log_error "Failed to clean build"
            exit 1
        }

        log_verbose "Building tests with ${_ncpu} parallel jobs..."
        confirm_cmd "Compile $_ct_suite tests in $TESTS_DIR (${_ncpu} parallel jobs)" \
            "make -j${_ncpu}" || return 1
        make -j"$_ncpu" || {
            log_error "Failed to build tests"
            exit 1
        }

        log_verbose "Installing tests..."
        confirm_cmd "Install test binaries to $TESTS_INSTALL_DIR" \
            "make install" || return 1
        make install || {
            log_error "Failed to install tests"
            exit 1
        }

        # The suite Makefile owns the installed Kyuafile.  Do not synthesize a
        # stale fallback here; run.sh category filtering expects the installed
        # syntax-2 atf_test_program entries to match the source-tree Kyuafile.
        if [ ! -f "$TESTS_INSTALL_DIR/Kyuafile" ]; then
            log_error "$_ct_suite install did not produce $TESTS_INSTALL_DIR/Kyuafile"
            exit 1
        fi

        log_success "$_ct_suite test suite built and installed successfully"
        log_info "Tests installed to: $TESTS_INSTALL_DIR"
    else
        log_info "Would build and install tests (dry run)"
    fi
}

# Commit tests and ci/tools to sos-git
commit_to_sos() {
    log_info "Committing to sos-git..."

    if [ ! -d "$SOS_DIR/.git" ]; then
        log_error "sos-git working tree not found: $SOS_DIR"
        log_error "Expected a git clone of ssh://git@sos-git.amd.com/freebsd-src.git"
        exit 1
    fi

    COMMIT_DATE=$(date +%Y-%m-%d)

    if [ $DRY_RUN -eq 0 ]; then
        confirm_cmd "Sync tests/ and ci/tools/ into $SOS_DIR, then commit and push to sos-git branch $SOS_BRANCH" \
            "cp tests/ ci/tools/ → $SOS_DIR  &&  git commit  &&  git push origin $SOS_BRANCH" || return 1

        # Sync tests/sys/amd/ibs/
        log_verbose "Syncing tests/sys/amd/ibs/ ..."
        mkdir -p "$SOS_DIR/tests/sys/amd/ibs"
        cp -r "$SCRIPT_DIR/tests/sys/amd/ibs/." "$SOS_DIR/tests/sys/amd/ibs/" || {
            log_error "Failed to sync tests/sys/amd/ibs/"
            exit 1
        }

        # Sync tests/sys/amd/pmc/
        log_verbose "Syncing tests/sys/amd/pmc/ ..."
        mkdir -p "$SOS_DIR/tests/sys/amd/pmc"
        cp -r "$SCRIPT_DIR/tests/sys/amd/pmc/." "$SOS_DIR/tests/sys/amd/pmc/" || {
            log_error "Failed to sync tests/sys/amd/pmc/"
            exit 1
        }

        # Sync ci/tools/
        log_verbose "Syncing ci/tools/ ..."
        mkdir -p "$SOS_DIR/ci/tools"
        cp -r "$SCRIPT_DIR/ci/tools/." "$SOS_DIR/ci/tools/" || {
            log_error "Failed to sync ci/tools/"
            exit 1
        }

        cd "$SOS_DIR" || exit 1

        # Stage only tests and ci/tools -no kernel files, no personal docs
        git add tests/sys/amd/ibs/ tests/sys/amd/pmc/ ci/tools/

        # Check if there is anything new to commit
        if git diff --cached --quiet; then
            log_info "Nothing to commit -sos-git is already up to date"
            return 0
        fi

        COMMIT_MSG="amd: update tests and ci tools -$COMMIT_DATE"
        git commit -m "$COMMIT_MSG" || {
            log_error "Failed to create commit"
            exit 1
        }
        log_verbose "Committed: $COMMIT_MSG"

        confirm_cmd "Push to remote sos-git (irreversible)" \
            "git push origin $SOS_BRANCH" || return 1
        git push origin "HEAD:$SOS_BRANCH" || {
            log_error "Failed to push to sos-git"
            exit 1
        }
        log_success "Pushed to ssh://git@sos-git.amd.com/freebsd-src.git branch $SOS_BRANCH"
    else
        log_info "Would sync: tests/sys/amd/ibs/, tests/sys/amd/pmc/, ci/tools/"
        log_info "Would commit: amd: update tests and ci tools -$COMMIT_DATE"
        log_info "Would push to: ssh://git@sos-git.amd.com/freebsd-src.git branch $SOS_BRANCH"
    fi
}

# Fetch / push the freebsd-ci-actions repo itself (origin = github.com/ojanerif/freebsd-ci-actions)
fetch_from_remote() {
    log_info "Fetching from origin (github.com/ojanerif/freebsd-ci-actions main)..."

    if [ ! -d "$SCRIPT_DIR/.git" ]; then
        log_error "Not a git repository: $SCRIPT_DIR"
        exit 1
    fi

    if [ $DRY_RUN -eq 0 ]; then
        _before=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null)
        confirm_cmd "Pull origin main into $SCRIPT_DIR" \
            "git -C $SCRIPT_DIR pull origin main" || return 1
        git -C "$SCRIPT_DIR" pull origin main || {
            log_error "Failed to pull from origin"
            exit 1
        }
        _after=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null)
        if [ "$_before" = "$_after" ]; then
            log_info "Already up to date ($_before)"
        else
            log_success "Updated $_before → $_after"
            git -C "$SCRIPT_DIR" log --oneline "${_before}..HEAD" 2>/dev/null | \
                while IFS= read -r line; do printf "  %s\n" "$line"; done
        fi
    else
        log_info "Would pull from origin main (dry run)"
    fi
}

push_to_remote() {
    log_info "Pushing to origin (github.com/ojanerif/freebsd-ci-actions main)..."

    if [ ! -d "$SCRIPT_DIR/.git" ]; then
        log_error "Not a git repository: $SCRIPT_DIR"
        exit 1
    fi

    if [ $DRY_RUN -eq 0 ]; then
        # Fetch remote state so the ahead count is current
        git -C "$SCRIPT_DIR" fetch origin main 2>/dev/null || true
        _ahead=$(git -C "$SCRIPT_DIR" rev-list --count origin/main..HEAD 2>/dev/null || echo 0)
        if [ "${_ahead:-0}" -eq 0 ]; then
            log_info "Nothing to push -local main is already up to date with origin/main"
            return 0
        fi
        log_info "$_ahead commit(s) ahead of origin/main:"
        git -C "$SCRIPT_DIR" log --oneline origin/main..HEAD 2>/dev/null | \
            while IFS= read -r line; do printf "  %s\n" "$line"; done
        confirm_cmd "Push local main to origin (irreversible)" \
            "git -C $SCRIPT_DIR push origin main" || return 1
        git -C "$SCRIPT_DIR" push origin main || {
            log_error "Failed to push to origin"
            exit 1
        }
        log_success "Pushed $_ahead commit(s) to origin/main"
    else
        log_info "Would push to origin main (dry run)"
    fi
}

# HTML helpers -used by generate_html_report and generate_html_index
_he() { printf '%s' "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'; }

generate_html_report() {
    # All summary globals (TOTAL_TESTS, PASSED_TESTS …) and TMP_MATRIX must be in scope.
    _hr_file="$RESULTS_DIR/report.html"
    _run_name=$(basename "$RESULTS_DIR")

    # Verdict string + CSS class
    if [ "$VERDICT_FAIL" -eq 0 ]; then
        if [ "$MED_PASS_PCT" -lt 0 ] || [ "$MED_PASS_PCT" -ge 80 ]; then
            _vtext="APPROVED"; _vcls="v-approved"
        else
            _vtext="CONDITIONAL"; _vcls="v-conditional"
        fi
    else
        _vtext="NOT APPROVED"; _vcls="v-rejected"
    fi

    # Overall pass rate (excl. skip/xfail)
    _hr_denom=$((PASSED_TESTS + FAILED_TESTS + BROKEN_TESTS))
    [ "$_hr_denom" -gt 0 ] && _hr_rate=$(( PASSED_TESTS * 100 / _hr_denom )) || _hr_rate=0

    # Severity HIGH / MEDIUM strings
    if [ "$HIGH_PASS_PCT" -lt 0 ]; then
        _high_str="N/A"; _high_cls=""
    elif [ "$HIGH_PASS_PCT" -ge 95 ]; then
        _high_str="${HIGH_PASS_PCT}% &mdash; PASS"; _high_cls="sc-pass"
    else
        _high_str="${HIGH_PASS_PCT}% &mdash; FAIL"; _high_cls="sc-fail"
    fi
    if [ "$MED_PASS_PCT" -lt 0 ]; then
        _med_str="N/A"; _med_cls=""
    elif [ "$MED_PASS_PCT" -ge 80 ]; then
        _med_str="${MED_PASS_PCT}% &mdash; PASS"; _med_cls="sc-pass"
    else
        _med_str="${MED_PASS_PCT}% &mdash; FAIL"; _med_cls="sc-fail"
    fi
    if [ "$CRIT_FAIL" -eq 0 ]; then
        _crit_str="0 failed &mdash; PASS"; _crit_cls="sc-pass"
    else
        _crit_str="${CRIT_FAIL} failed &mdash; FAIL"; _crit_cls="sc-fail"
    fi

    # Diagnostic rows for non-passed tests (shown in Diagnostics section below matrix)
    _diag_rows=""
    _TAB="$(printf '\t')"
    while IFS='|' read -r _tc _cat _sev _st _tm; do
        case "$_st" in passed) continue ;; esac
        _dtce=$(_he "$_tc")
        _diag_info=$(diag_for_test "$_tc")
        _dclass=$(printf '%s' "$_diag_info"  | grep '^CLASS:'     | sed 's/^CLASS://')
        _dop=$(printf '%s'    "$_diag_info"  | grep '^OPERATION:' | sed 's/^OPERATION://')
        _dcause=$(printf '%s' "$_diag_info"  | grep '^CAUSE:'     | sed 's/^CAUSE://')
        _daction=$(printf '%s' "$_diag_info" | grep '^ACTION:'    | sed 's/^ACTION://')
        _dope=$(_he "$_dop"); _dcausee=$(_he "$_dcause"); _dactione=$(_he "$_daction")
        case "$_st" in
            failed)  _dbadge="<span class=\"badge r-fail\">FAIL</span>";   _dbg="diag-fail" ;;
            broken)  _dbadge="<span class=\"badge r-broken\">BRKN</span>"; _dbg="diag-broken" ;;
            skipped)
                if [ "$_dclass" = "normal" ]; then
                    _dbadge="<span class=\"badge r-skip\">SKIP-OK</span>"; _dbg="diag-skip-ok"
                else
                    _dbadge="<span class=\"badge r-skip\">SKIP</span>";    _dbg="diag-skip"
                fi ;;
            *)       _dbadge="<span class=\"badge\">${_st}</span>";        _dbg="" ;;
        esac
        _diag_rows="${_diag_rows}
<details class=\"diag-block ${_dbg}\">
  <summary>${_dbadge} <code>${_dtce}</code></summary>
  <div class=\"diag-body\">
    <table class=\"diag-table\">
$([ -n "$_dop"     ] && printf '    <tr><th>Operation</th><td>%s</td></tr>\n' "$_dope")
$([ -n "$_dcause"  ] && printf '    <tr><th>Root cause</th><td>%s</td></tr>\n' "$_dcausee")
$([ -n "$_daction" ] && [ "$_daction" != "" ] && printf '    <tr><th>Action</th><td>%s</td></tr>\n' "$_dactione")
    </table>
  </div>
</details>"
    done < "$TMP_MATRIX"

    # Matrix rows from TMP_MATRIX (pipe-delimited: tc|cat|sev|status|dur)
    # Descriptions are looked up from TMP_DESC (tc<TAB>description) if available.
    _mrows=""
    while IFS='|' read -r _tc _cat _sev _st _tm; do
        _tce=$(_he "$_tc")
        # Look up ATF description for tooltip; gracefully absent if TMP_DESC missing.
        _desc=""
        if [ -n "${TMP_DESC:-}" ] && [ -f "$TMP_DESC" ]; then
            _desc=$(grep -m1 "^${_tc}${_TAB}" "$TMP_DESC" 2>/dev/null | cut -f2-)
        fi
        _desce=$(_he "$_desc")
        case "$_st" in
            passed)  _rcls="r-pass";   _slbl="PASS"  ;;
            failed)  _rcls="r-fail";   _slbl="FAIL"  ;;
            skipped) _rcls="r-skip";   _slbl="SKIP"  ;;
            xfail)   _rcls="r-xfail";  _slbl="XFAIL" ;;
            broken)  _rcls="r-broken"; _slbl="BRKN"  ;;
            *)       _rcls="";         _slbl="$_st"  ;;
        esac
        case "$_sev" in
            CRITICAL) _sevcls="s-crit" ;;
            HIGH)     _sevcls="s-high" ;;
            MEDIUM)   _sevcls="s-med"  ;;
            *)        _sevcls=""       ;;
        esac
        # Derive execution phase/batch from category code
        case "$_cat" in
            TC-CSTR|TC-ISTR|TC-FSTR|TC-STR|TC-NMISTR) _phase="Stress B1·CPU"   ; _phcls="ph-s1" ;;
            TC-MSTR|TC-MEMIBS)                          _phase="Stress B2·Mem"   ; _phcls="ph-s2" ;;
            TC-DSTR)                                    _phase="Stress B3·Disk"  ; _phcls="ph-s3" ;;
            TC-NSTR)                                    _phase="Stress B4·Net"   ; _phcls="ph-s4" ;;
            *)                                          _phase="Ph1·Non-stress"  ; _phcls="ph-1"  ;;
        esac
        _mrows="${_mrows}<tr class=\"${_rcls}\"><td class=\"tc\" data-desc=\"${_desce}\" onclick=\"toggleDesc(this)\"><span class=\"tc-name\">${_tce}</span></td><td>${_cat}</td><td class=\"${_sevcls}\">${_sev}</td><td><span class=\"badge ${_rcls}\">${_slbl}</span></td><td class=\"dur\">${_tm}</td><td class=\"phase ${_phcls}\">${_phase}</td></tr>
"
    done < "$TMP_MATRIX"

    _gen_date=$(_he "$(date)")
    _sys_esc=$(_he "$(uname -a)")

    cat > "$_hr_file" << HTMLEOF
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AMD IBS CI &mdash; ${_run_name}</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#e6edf3;font-family:'Segoe UI',system-ui,sans-serif;font-size:14px;line-height:1.5}
a{color:#58a6ff;text-decoration:none}a:hover{text-decoration:underline}
header{background:#161b22;border-bottom:1px solid #30363d;padding:20px 32px}
header h1{font-size:20px;font-weight:600;color:#e6edf3}
header .meta{color:#8b949e;font-size:12px;margin-top:4px;font-family:monospace}
.container{max-width:1400px;margin:0 auto;padding:24px 32px}
.verdict-banner{border-radius:8px;padding:16px 24px;margin-bottom:24px;display:flex;align-items:center;gap:16px}
.v-approved{background:#0d4a1f;border:1px solid #3fb950}
.v-conditional{background:#3d2b00;border:1px solid #d29922}
.v-rejected{background:#3d0b0b;border:1px solid #f85149}
.verdict-text{font-size:22px;font-weight:700;letter-spacing:1px}
.v-approved .verdict-text{color:#3fb950}
.v-conditional .verdict-text{color:#d29922}
.v-rejected .verdict-text{color:#f85149}
.verdict-sub{color:#8b949e;font-size:13px}
.stats-row{display:grid;grid-template-columns:repeat(6,1fr);gap:12px;margin-bottom:24px}
.stat-card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px 16px;text-align:center}
.stat-card .num{font-size:28px;font-weight:700;line-height:1.1}
.stat-card .lbl{font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;margin-top:4px}
.c-total .num{color:#e6edf3}
.c-pass .num{color:#3fb950}
.c-fail .num{color:#f85149}
.c-skip .num{color:#d29922}
.c-broken .num{color:#db6d28}
.c-rate .num{color:#58a6ff}
.section-title{font-size:13px;font-weight:600;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;margin-bottom:10px}
.sev-table{width:100%;border-collapse:collapse;margin-bottom:24px;background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden}
.sev-table th{background:#21262d;padding:10px 14px;text-align:left;font-size:12px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;font-weight:500}
.sev-table td{padding:10px 14px;border-top:1px solid #30363d}
.sc-pass{color:#3fb950;font-weight:600}
.sc-fail{color:#f85149;font-weight:600}
.s-crit{color:#f85149;font-weight:700}
.s-high{color:#d29922;font-weight:600}
.s-med{color:#58a6ff;font-weight:600}
.matrix-wrap{overflow-x:auto;margin-bottom:24px}
table.matrix{width:100%;border-collapse:collapse;background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden}
table.matrix th{background:#21262d;padding:9px 12px;text-align:left;font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;font-weight:500;position:sticky;top:0}
table.matrix td{padding:7px 12px;border-top:1px solid #21262d;font-family:monospace;font-size:12px}
table.matrix td.tc{max-width:440px;cursor:pointer}
.tc-name{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tc-desc{font-family:'Segoe UI',system-ui,sans-serif;font-size:11px;color:#8b949e;white-space:normal;margin-top:4px;padding-top:4px;border-top:1px dotted #30363d;line-height:1.4}
table.matrix td.dur{color:#8b949e;white-space:nowrap}
tr.r-fail{background:#1f0e0e}
tr.r-broken{background:#1f1400}
tr.r-skip{background:#161b22;opacity:.7}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:700;font-family:monospace}
.badge.r-pass{background:#0d4a1f;color:#3fb950}
.badge.r-fail{background:#3d0b0b;color:#f85149}
.badge.r-skip{background:#2d2700;color:#d29922}
.badge.r-broken{background:#2d1800;color:#db6d28}
.badge.r-xfail{background:#0d2a3d;color:#58a6ff}
footer{background:#161b22;border-top:1px solid #30363d;padding:16px 32px;color:#8b949e;font-size:12px;display:flex;gap:24px;align-items:center}
.diag-block{background:#161b22;border:1px solid #30363d;border-radius:6px;margin-bottom:8px;overflow:hidden}
.diag-block summary{padding:10px 14px;cursor:pointer;font-family:monospace;font-size:12px;list-style:none;display:flex;align-items:center;gap:10px;color:#e6edf3}
.diag-block summary::-webkit-details-marker{display:none}
.diag-block summary::before{content:"▶";font-size:10px;color:#8b949e;transition:transform .15s}
.diag-block[open] summary::before{transform:rotate(90deg)}
.diag-block.diag-fail{border-color:#f85149}
.diag-block.diag-broken{border-color:#db6d28}
.diag-block.diag-skip{border-color:#d29922}
.diag-block.diag-skip-ok{opacity:.6}
.diag-body{padding:12px 16px;border-top:1px solid #30363d}
.diag-table{width:100%;border-collapse:collapse;font-size:12px}
.diag-table th{width:110px;padding:6px 10px 6px 0;color:#8b949e;font-weight:500;vertical-align:top;white-space:nowrap}
.diag-table td{padding:6px 0;color:#e6edf3;line-height:1.5}
td.phase{font-size:11px;white-space:nowrap;font-family:monospace}
.ph-1  {color:#8b949e}
.ph-s1 {color:#f0883e}
.ph-s2 {color:#79c0ff}
.ph-s3 {color:#d2a8ff}
.ph-s4 {color:#56d364}
.phase-legend{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:24px}
.phase-pill{padding:4px 12px;border-radius:20px;font-size:12px;font-weight:600;font-family:monospace}
.pill-1 {background:#21262d;color:#8b949e;border:1px solid #30363d}
.pill-s1{background:#2d1a00;color:#f0883e;border:1px solid #f0883e}
.pill-s2{background:#00213d;color:#79c0ff;border:1px solid #79c0ff}
.pill-s3{background:#1e0a3d;color:#d2a8ff;border:1px solid #d2a8ff}
.pill-s4{background:#0d2a14;color:#56d364;border:1px solid #56d364}
</style>
</head>
<body>
<header>
  <h1>AMD IBS CI &mdash; Test Run Report</h1>
  <div class="meta">Run: ${_run_name} &nbsp;&bull;&nbsp; Generated: ${_gen_date}</div>
  <div class="meta">${_sys_esc}</div>
</header>
<div class="container">

<div class="verdict-banner ${_vcls}">
  <div class="verdict-text">VERDICT: ${_vtext}</div>
  <div class="verdict-sub">Pass rate (excl. skip): ${_hr_rate}% &nbsp;&bull;&nbsp; Parallelism: ${PARALLELISM}</div>
</div>

<div class="stats-row">
  <div class="stat-card c-total"><div class="num">${TOTAL_TESTS}</div><div class="lbl">Total</div></div>
  <div class="stat-card c-pass"><div class="num">${PASSED_TESTS}</div><div class="lbl">Passed</div></div>
  <div class="stat-card c-fail"><div class="num">${FAILED_TESTS}</div><div class="lbl">Failed</div></div>
  <div class="stat-card c-skip"><div class="num">${SKIPPED_TESTS}</div><div class="lbl">Skipped</div></div>
  <div class="stat-card c-broken"><div class="num">${BROKEN_TESTS}</div><div class="lbl">Broken</div></div>
  <div class="stat-card c-rate"><div class="num">${_hr_rate}%</div><div class="lbl">Pass Rate</div></div>
</div>

<div class="section-title">Severity Criteria</div>
<table class="sev-table">
<thead><tr><th>Severity</th><th>Threshold</th><th>Result</th></tr></thead>
<tbody>
<tr><td class="s-crit">CRITICAL</td><td>0 failures required</td><td class="${_crit_cls}">${_crit_str}</td></tr>
<tr><td class="s-high">HIGH</td><td>&ge; 95% pass rate required</td><td class="${_high_cls}">${_high_str}</td></tr>
<tr><td class="s-med">MEDIUM</td><td>&ge; 80% pass rate required</td><td class="${_med_cls}">${_med_str}</td></tr>
</tbody>
</table>

<div class="section-title">Execution Phases</div>
<div class="phase-legend">
  <span class="phase-pill pill-1">Phase 1 &mdash; Non-stress (suite-safe parallelism)</span>
  <span class="phase-pill pill-s1">Stress Batch 1 &mdash; CPU</span>
  <span class="phase-pill pill-s2">Stress Batch 2 &mdash; Memory</span>
  <span class="phase-pill pill-s3">Stress Batch 3 &mdash; Disk</span>
  <span class="phase-pill pill-s4">Stress Batch 4 &mdash; Network</span>
</div>

<div class="section-title">Test Results Matrix</div>
<div class="matrix-wrap">
<table class="matrix">
<thead><tr><th>Test Case</th><th>Category</th><th>Severity</th><th>Status</th><th>Duration</th><th>Phase</th></tr></thead>
<tbody>
${_mrows}
</tbody>
</table>
</div>

<div class="section-title">Diagnostics — Non-Passed Tests</div>
<div style="margin-bottom:24px">
${_diag_rows}
</div>

</div>
<footer>
  <a href="report.txt">Raw report (.txt)</a>
  <a href="report.xml">JUnit XML (.xml)</a>
  <a href="../">&#8592; All runs</a>
</footer>
<script>
function toggleDesc(el){
  var d=el.querySelector('.tc-desc');
  if(d){d.remove();return;}
  var desc=el.dataset.desc;
  if(!desc)return;
  d=document.createElement('div');
  d.className='tc-desc';
  d.textContent=desc;
  el.appendChild(d);
}
</script>
</body>
</html>
HTMLEOF

    log_success "HTML report  : ${_hr_file}"
}

generate_html_index() {
    _idx_file="$HTML_DIR/index.html"
    _hostname=$(hostname)
    _now=$(date)

    # Scan all results dirs, newest first by name (datetime names sort correctly)
    _rows=""
    # Use find + sort (reverse alpha = newest first since names are timestamped)
    # to avoid word-splitting on dir names with embedded whitespace.
    find "$HTML_DIR" -maxdepth 1 -type d -name 'results-*' 2>/dev/null \
        | sort -r > /tmp/ibs_idx_$$.tmp
    while IFS= read -r _d; do
        [ -d "$_d" ] || continue
        _n=$(basename "$_d")
        _rpt="$_d/report.txt"
        [ -f "$_rpt" ] || continue
        [ -f "$_d/report.html" ] || continue

        _date=$(grep "^Generated  :" "$_rpt" 2>/dev/null | sed 's/Generated  : //' | head -1)
        [ -z "$_date" ] && _date="—"

        _verdict=$(grep "^VERDICT:" "$_rpt" 2>/dev/null | head -1 | sed 's/VERDICT: //')
        _verdict_short=$(printf '%s' "$_verdict" | sed 's/ [-—].*//')
        case "$_verdict_short" in
            APPROVED)     _vc="v-approved"    ;;
            CONDITIONAL)  _vc="v-conditional" ;;
            SKIPPED)      _vc="v-skipped"     ;;
            *)            _vc="v-rejected"    ;;
        esac

        _total=$(awk '/^Tests Run :/{print $NF}' "$_rpt" 2>/dev/null | head -1)
        _pass=$(awk '/^Passed    :/{print $3}' "$_rpt" 2>/dev/null | head -1)
        _fail=$(awk '/^Failed    :/{print $3}' "$_rpt" 2>/dev/null | head -1)
        _skip=$(awk '/^Skipped   :/{print $3}' "$_rpt" 2>/dev/null | head -1)
        _brkn=$(awk '/^Broken    :/{print $3}' "$_rpt" 2>/dev/null | head -1)
        _pct=$(grep "^Passed    :" "$_rpt" 2>/dev/null | sed 's/.*(\([0-9]*\)%).*/\1/' | head -1)
        [ -z "$_total" ] && _total="—"
        [ -z "$_pass"  ] && _pass="—"
        [ -z "$_fail"  ] && _fail="—"
        [ -z "$_skip"  ] && _skip="—"
        [ -z "$_brkn"  ] && _brkn="—"
        [ -n "$_pct" ] && _pct_disp="${_pct}%" || _pct_disp="—"

        # Extract kernel string: "FreeBSD 16.0-CURRENT #N branch-slug" (strip hostname+release dupe, cut at build date)
        _kver=$(grep "^System     :" "$_rpt" 2>/dev/null | head -1 | sed 's/System     : FreeBSD [^ ]* [^ ]* //' | awk -F': ' '{print $1}' | cut -c1-60)
        [ -z "$_kver" ] && _kver="—"
        _kver_esc=$(_he "$_kver")

        _has_html=""; [ -f "$_d/report.html" ] && _has_html=" <a href=\"${_n}/report.html\">html</a>"

        _rows="${_rows}<tr>
<td><a href=\"${_n}/report.html\">${_n}</a></td>
<td class=\"dt\">${_date}</td>
<td><span class=\"vbadge ${_vc}\">${_verdict_short}</span></td>
<td class=\"kver\" title=\"${_kver_esc}\">${_kver_esc}</td>
<td class=\"num\">${_pct_disp}</td>
<td class=\"num\">${_total}</td>
<td class=\"num c-pass\">${_pass}</td>
<td class=\"num c-fail\">${_fail}</td>
<td class=\"num c-skip\">${_skip}</td>
<td class=\"num c-broken\">${_brkn}</td>
<td class=\"links\"><a href=\"${_n}/report.txt\">txt</a>${_has_html} <a href=\"${_n}/report.xml\">xml</a></td>
</tr>
"
    done < /tmp/ibs_idx_$$.tmp
    rm -f /tmp/ibs_idx_$$.tmp

    cat > "$_idx_file" << IDXEOF
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="60">
<title>AMD IBS CI &mdash; Test Run History</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#e6edf3;font-family:'Segoe UI',system-ui,sans-serif;font-size:14px;line-height:1.5}
a{color:#58a6ff;text-decoration:none}a:hover{text-decoration:underline}
header{background:#161b22;border-bottom:1px solid #30363d;padding:20px 32px;display:flex;align-items:baseline;gap:16px}
header h1{font-size:20px;font-weight:600}
header .sub{color:#8b949e;font-size:12px}
.container{max-width:1400px;margin:0 auto;padding:24px 32px}
.refresh-note{color:#8b949e;font-size:11px;margin-bottom:16px}
table{width:100%;border-collapse:collapse;background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden}
th{background:#21262d;padding:10px 12px;text-align:left;font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;font-weight:500}
td{padding:9px 12px;border-top:1px solid #21262d;font-size:13px}
td.dt{font-family:monospace;font-size:12px;color:#8b949e;white-space:nowrap}
td.num{text-align:right;font-family:monospace;font-weight:600}
td.kver{font-family:monospace;font-size:12px;color:#8b949e;white-space:nowrap;max-width:260px;overflow:hidden;text-overflow:ellipsis}
td.links{font-size:12px;white-space:nowrap}
td.c-pass{color:#3fb950}
td.c-fail{color:#f85149}
td.c-skip{color:#d29922}
td.c-broken{color:#db6d28}
tr:hover{background:#1c2128}
.vbadge{display:inline-block;padding:3px 10px;border-radius:4px;font-size:11px;font-weight:700;letter-spacing:.3px}
.v-approved{background:#0d4a1f;color:#3fb950;border:1px solid #3fb950}
.v-conditional{background:#3d2b00;color:#d29922;border:1px solid #d29922}
.v-rejected{background:#3d0b0b;color:#f85149;border:1px solid #f85149}
.v-skipped{background:#1c2332;color:#79c0ff;border:1px solid #388bfd}
footer{background:#161b22;border-top:1px solid #30363d;padding:14px 32px;color:#8b949e;font-size:12px}
</style>
</head>
<body>
<header>
  <h1>AMD IBS CI &mdash; Test Run History</h1>
  <div class="sub">${_hostname} &nbsp;&bull;&nbsp; Last updated: ${_now}</div>
</header>
<div class="container">
<div class="refresh-note">Page auto-refreshes every 60 s</div>
<table>
<thead>
<tr>
  <th>Run</th><th>Date / Time</th><th>Verdict</th><th>Kernel</th>
  <th style="text-align:right">Pass%</th>
  <th style="text-align:right">Total</th>
  <th style="text-align:right">Pass</th>
  <th style="text-align:right">Fail</th>
  <th style="text-align:right">Skip</th>
  <th style="text-align:right">Brkn</th>
  <th>Files</th>
</tr>
</thead>
<tbody>
${_rows}
</tbody>
</table>
</div>
<footer>FreeBSD IBS CI &mdash; darkhttpd &mdash; ${_hostname}</footer>
</body>
</html>
IDXEOF

    log_success "HTML index   : ${_idx_file}"
}

# Generate a "SKIPPED -no kernel changes" HTML report + update the index.
# Called by auto_mode when the remote commit is already the last-tested commit.
# Args: $1=commit  $2=branch
generate_html_skipped_report() {
    _sk_commit="${1:-unknown}"
    _sk_branch="${2:-unknown}"
    _sk_ts=$(date +%Y-%m-%d_%H-%M-%S)
    _sk_dir="$HTML_DIR/results-${_sk_ts}-skipped"
    _sk_txt="$_sk_dir/report.txt"
    _sk_html="$_sk_dir/report.html"
    _sk_date=$(date)
    _sk_run=$(basename "$_sk_dir")

    mkdir -p "$_sk_dir"

    # Plain-text report -format matches what generate_html_index parses
    cat > "$_sk_txt" << SKIPTXTEOF
AMD PMU CI -Kernel Up To Date
Suite      : ${SUITE}
Kernel     : ${AUTO_KERNCONF}
Host       : $(uname -n)  ($(uname -r))
Generated  : ${_sk_date}

VERDICT: SKIPPED -No kernel changes since last tested commit

No new commits on branch ${_sk_branch} since the last test run.
Commit     : ${_sk_commit}
Repo       : ${REPO_URL}

Nothing to do -no build or reboot was started.
SKIPTXTEOF

    # Minimal JUnit XML so the index links don't 404
    cat > "$_sk_dir/report.xml" << SKIPXMLEOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuites><testsuite name="skipped" tests="0" skipped="0" failures="0" errors="0"/></testsuites>
SKIPXMLEOF

    _sk_date_esc=$(_he "$_sk_date")
    _sk_sys_esc=$(_he "$(uname -srm)")
    _sk_commit_esc=$(_he "$_sk_commit")
    _sk_branch_esc=$(_he "$_sk_branch")
    _sk_repo_esc=$(_he "$REPO_URL")
    _sk_host_esc=$(_he "$(hostname) ($(uname -r))")

    cat > "$_sk_html" << SKIPHTMLEOF
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AMD IBS CI &mdash; ${_sk_run}</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#e6edf3;font-family:'Segoe UI',system-ui,sans-serif;font-size:14px;line-height:1.5}
a{color:#58a6ff;text-decoration:none}a:hover{text-decoration:underline}
header{background:#161b22;border-bottom:1px solid #30363d;padding:20px 32px}
header h1{font-size:20px;font-weight:600;color:#e6edf3}
header .meta{color:#8b949e;font-size:12px;margin-top:4px;font-family:monospace}
.container{max-width:900px;margin:0 auto;padding:24px 32px}
.verdict-banner{border-radius:8px;padding:20px 24px;margin-bottom:24px;background:#1c2332;border:1px solid #388bfd}
.verdict-text{font-size:22px;font-weight:700;letter-spacing:1px;color:#79c0ff}
.verdict-sub{color:#8b949e;font-size:13px;margin-top:6px}
.info-table{width:100%;border-collapse:collapse;background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden;margin-bottom:24px}
.info-table td{padding:10px 16px;border-top:1px solid #21262d;font-size:13px}
.info-table td:first-child{color:#8b949e;width:140px;font-size:11px;text-transform:uppercase;letter-spacing:.5px;white-space:nowrap}
.info-table td:last-child{font-family:monospace}
footer{background:#161b22;border-top:1px solid #30363d;padding:16px 32px;color:#8b949e;font-size:12px;display:flex;gap:24px;align-items:center}
</style>
</head>
<body>
<header>
  <h1>AMD IBS CI &mdash; Test Run Report</h1>
  <div class="meta">Run: ${_sk_run} &nbsp;&bull;&nbsp; Generated: ${_sk_date_esc} &nbsp;&bull;&nbsp; ${_sk_sys_esc}</div>
</header>
<div class="container">

<div class="verdict-banner">
  <div class="verdict-text">SKIPPED &mdash; No kernel changes</div>
  <div class="verdict-sub">The kernel is already up to date. No build, install, or reboot was started.</div>
</div>

<table class="info-table">
<tbody>
<tr><td>Branch</td><td>${_sk_branch_esc}</td></tr>
<tr><td>Commit</td><td>${_sk_commit_esc}</td></tr>
<tr><td>Repo</td><td>${_sk_repo_esc}</td></tr>
<tr><td>Suite</td><td>${SUITE}</td></tr>
<tr><td>Kernel conf</td><td>${AUTO_KERNCONF}</td></tr>
<tr><td>Host</td><td>${_sk_host_esc}</td></tr>
<tr><td>Generated</td><td>${_sk_date_esc}</td></tr>
</tbody>
</table>

</div>
<footer>
  <a href="report.txt">Raw report (.txt)</a>
  <a href="../">&#8592; All runs</a>
</footer>
</body>
</html>
SKIPHTMLEOF

    generate_html_index
    log_success "Skipped HTML report: ${_sk_html}"
}

# Validate that a suite has an installed Kyua directory and Kyuafile.
validate_installed_suite() {
    _vis_suite="$1"
    if ! _vis_dir=$(suite_install_dir "$_vis_suite"); then
        log_error "Unknown suite: $_vis_suite"
        return 1
    fi
    if [ ! -d "$_vis_dir" ]; then
        log_error "[$_vis_suite] Tests not installed at $_vis_dir. Run --suite $_vis_suite --compile first."
        return 1
    fi
    if [ ! -f "$_vis_dir/Kyuafile" ]; then
        log_error "[$_vis_suite] Kyuafile missing at $_vis_dir/Kyuafile. Run --suite $_vis_suite --compile first."
        return 1
    fi
    return 0
}

# Run kyua for a single suite, accumulating results into shared temp files.
# Args: $1 = suite name (IBS|UMCDF|PMC|TSC|L3|STRESS)
# Globals written: TMP_PASS, TMP_FAIL, TMP_SKIP, TMP_XFAIL, TMP_BROKEN,
#                  TMP_CRIT_F, TMP_HIGH_P, TMP_HIGH_F, TMP_MED_P, TMP_MED_F,
#                  TMP_MATRIX, TMP_DESC, LAST_RUN_LOG, LAST_TEST_STATE,
#                  REPORT_TXT (kyua verbose appended)
# Returns: kyua exit code (0 = all tests passed/skipped, non-zero = failures)
_run_suite_once() {
    _rs_suite="$1"
    SUITE="$_rs_suite"
    if ! validate_installed_suite "$_rs_suite"; then
        return 1
    fi
    TESTS_INSTALL_DIR=$(suite_install_dir "$_rs_suite")

    # Parallelism: all suites and stress batches now respect $PARALLELISM.
    # Resource isolation is achieved by running stress batches sequentially
    # (Batch 1 CPU → Batch 2 Memory → Batch 3 Disk → Batch 4 Network) via
    # _run_stress_batches(), not by forcing parallelism=1 here.
    #
    # Exception — PMC suite is capped at parallelism=1.
    # pmcstat_grouping_test cases use `pmcstat -c 0` (system-wide counting),
    # which allocates PMC_MODE_SC rows on every CPU simultaneously.  Running
    # multiple such tests concurrently causes a kernel panic or hard reboot due
    # to concurrent system-wide pmc_allocate()/pmc_start() contention inside
    # hwpmc.ko.  Observed on a 192-CPU EPYC machine: reboot with no crash dump.
    _rs_par="$PARALLELISM"
    if [ "$_rs_suite" = "PMC" ] && [ "$_rs_par" -gt 1 ] 2>/dev/null; then
        log_warning "PMC suite: capping parallelism from ${_rs_par} to 1 (system-wide PMC tests unsafe under concurrency)"
        _rs_par=1
    fi

    echo ""
    echo "================================================================="
    printf "SUITE: %s — LIVE OUTPUT  (parallelism: %s)\n" "$_rs_suite" "$_rs_par"
    echo "================================================================="

    printf '\n=== SUITE: %s started: %s ===\n' "$_rs_suite" "$(date)" >> "$LAST_RUN_LOG"
    [ -f "$LAST_TEST_STATE" ] && cp "$LAST_TEST_STATE" "${LAST_TEST_STATE}.prev"
    printf 'SUITE=%s\nSTARTED=%s\nPARALLELISM=%s\nLAST_COMPLETED=\nLAST_STATUS=\nLAST_SEEN_AT=\n' \
        "$_rs_suite" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$_rs_par" > "$LAST_TEST_STATE"

    cd "$TESTS_INSTALL_DIR" || return 1

    # Category filter
    _rs_kyuafile_opt=""
    _rs_cat_label=""
    _rs_kf=""
    if [ -n "$CATEGORIES" ]; then
        _rs_kf=$(build_filtered_kyuafile "$TESTS_INSTALL_DIR")
        if [ -n "$_rs_kf" ] && [ -f "$_rs_kf" ]; then
            _rs_kyuafile_opt="--kyuafile $_rs_kf"
            _rs_cat_label=" [categories: $CATEGORIES]"
        fi
    fi

    # Build description lookup for this suite (append to shared TMP_DESC)
    # shellcheck disable=SC2086
    kyua list --verbose${_rs_kyuafile_opt:+ $_rs_kyuafile_opt} 2>/dev/null | awk '
        /^[^[:space:]]/ { tc = $1; sub(/ .*$/, "", tc) }
        /^[[:space:]]+description[[:space:]]*=/ {
            d = $0
            sub(/^[[:space:]]+description[[:space:]]*=[[:space:]]*/, "", d)
            printf "%s\t%s\n", tc, d
        }
    ' >> "$TMP_DESC"

    # Stress monitor for the STRESS suite
    _rs_sm_pid=""
    if [ "$_rs_suite" = "STRESS" ]; then
        echo 0 > "$TMP_SM_DONE"
        printf '(starting)' > "$TMP_SM_LAST"
        _rs_sm_start=$(date +%s)
        _rs_sm_total=$(kyua list 2>/dev/null | wc -l | tr -d ' \t')
        [ -z "$_rs_sm_total" ] || [ "$_rs_sm_total" -le 0 ] && _rs_sm_total=12
        log_info "Stress monitor started (${_rs_sm_total} tests, sequential)"
        stress_monitor_loop "$_rs_sm_start" "$_rs_sm_total" "$TMP_SM_DONE" "$TMP_SM_LAST" &
        _rs_sm_pid=$!
        _STRESS_MON_PID="$_rs_sm_pid"
    fi

    # Confirm + run kyua
    confirm_cmd "Run${_rs_cat_label} $_rs_suite tests in $TESTS_INSTALL_DIR (parallelism: $_rs_par)" \
        "kyua -v parallelism=$_rs_par test${_rs_kyuafile_opt:+ $_rs_kyuafile_opt}" || {
        if [ -n "$_rs_sm_pid" ]; then
            kill "$_rs_sm_pid" 2>/dev/null
            _STRESS_MON_PID=""
        fi
        return 1
    }

    _rs_exit_f="/tmp/ibs_exit_${$}_${_rs_suite}.tmp"
    # shellcheck disable=SC2086
    { kyua -v parallelism="$_rs_par" test $_rs_kyuafile_opt 2>&1; echo $? > "$_rs_exit_f"; } | \
    tee -a "$LAST_RUN_LOG" | \
    while IFS= read -r line; do
        PREFIX=""
        SEV=""
        TESTID=""
        _TIME=""
        case "$line" in
            *" -> "*)
                BIN=$(printf '%s' "$line" | sed 's/:.*//' | sed 's|.*/||')
                META=$(get_test_meta "$BIN")
                CAT=$(printf '%s' "$META" | cut -d: -f1)
                SEV=$(printf '%s' "$META" | cut -d: -f3)
                SCOL=$(severity_color "$SEV")
                PREFIX="${SCOL}[${CAT}]${NC}${SCOL}[${SEV}]${NC} "
                TESTID=$(printf '%s' "$line" | sed 's/ *->.*//' | sed 's|.*/||' | sed 's/[[:space:]]*$//')
                _TIME=$(printf '%s' "$line" | grep -oE '\[[0-9]+\.[0-9]+s\]' | tr -d '[]')
                [ -z "$_TIME" ] && _TIME="—"
                ;;
        esac
        case "$line" in
            *" -> "*"passed"*)
                echo $(( $(cat "$TMP_PASS") + 1 )) > "$TMP_PASS"
                case "$SEV" in
                    HIGH)   echo $(( $(cat "$TMP_HIGH_P") + 1 )) > "$TMP_HIGH_P" ;;
                    MEDIUM) echo $(( $(cat "$TMP_MED_P")  + 1 )) > "$TMP_MED_P"  ;;
                esac
                printf '%s|%s|%s|%s|%s\n' "$TESTID" "$CAT" "$SEV" "passed" "$_TIME" >> "$TMP_MATRIX"
                printf 'LAST_COMPLETED=%s\nLAST_STATUS=passed\nLAST_SEEN_AT=%s\n' \
                    "$TESTID" "$(date -u +%H:%M:%SZ)" >> "$LAST_TEST_STATE"
                printf '%s\n' "${PREFIX}${GREEN}✓${NC} ${line}"
                ;;
            *" -> "*"failed"*)
                echo $(( $(cat "$TMP_FAIL") + 1 )) > "$TMP_FAIL"
                case "$SEV" in
                    CRITICAL) echo $(( $(cat "$TMP_CRIT_F") + 1 )) > "$TMP_CRIT_F" ;;
                    HIGH)     echo $(( $(cat "$TMP_HIGH_F") + 1 )) > "$TMP_HIGH_F" ;;
                    MEDIUM)   echo $(( $(cat "$TMP_MED_F")  + 1 )) > "$TMP_MED_F"  ;;
                esac
                printf '%s|%s|%s|%s|%s\n' "$TESTID" "$CAT" "$SEV" "failed" "$_TIME" >> "$TMP_MATRIX"
                printf 'LAST_COMPLETED=%s\nLAST_STATUS=failed\nLAST_SEEN_AT=%s\n' \
                    "$TESTID" "$(date -u +%H:%M:%SZ)" >> "$LAST_TEST_STATE"
                printf '%s\n' "${PREFIX}${RED}✗${NC} ${line}"
                ;;
            *" -> "*"skipped"*)
                echo $(( $(cat "$TMP_SKIP") + 1 )) > "$TMP_SKIP"
                printf '%s|%s|%s|%s|%s\n' "$TESTID" "$CAT" "$SEV" "skipped" "$_TIME" >> "$TMP_MATRIX"
                printf 'LAST_COMPLETED=%s\nLAST_STATUS=skipped\nLAST_SEEN_AT=%s\n' \
                    "$TESTID" "$(date -u +%H:%M:%SZ)" >> "$LAST_TEST_STATE"
                printf '%s\n' "${PREFIX}${YELLOW}⊘${NC} ${line}"
                ;;
            *" -> "*"expected_failure"*)
                echo $(( $(cat "$TMP_XFAIL") + 1 )) > "$TMP_XFAIL"
                printf '%s|%s|%s|%s|%s\n' "$TESTID" "$CAT" "$SEV" "xfail" "$_TIME" >> "$TMP_MATRIX"
                printf 'LAST_COMPLETED=%s\nLAST_STATUS=xfail\nLAST_SEEN_AT=%s\n' \
                    "$TESTID" "$(date -u +%H:%M:%SZ)" >> "$LAST_TEST_STATE"
                printf '%s\n' "${PREFIX}${CYAN}~${NC} ${line}"
                ;;
            *" -> "*"broken"*)
                echo $(( $(cat "$TMP_BROKEN") + 1 )) > "$TMP_BROKEN"
                case "$SEV" in
                    CRITICAL) echo $(( $(cat "$TMP_CRIT_F") + 1 )) > "$TMP_CRIT_F" ;;
                    HIGH)     echo $(( $(cat "$TMP_HIGH_F") + 1 )) > "$TMP_HIGH_F" ;;
                    MEDIUM)   echo $(( $(cat "$TMP_MED_F")  + 1 )) > "$TMP_MED_F"  ;;
                esac
                printf '%s|%s|%s|%s|%s\n' "$TESTID" "$CAT" "$SEV" "broken" "$_TIME" >> "$TMP_MATRIX"
                printf 'LAST_COMPLETED=%s\nLAST_STATUS=broken\nLAST_SEEN_AT=%s\n' \
                    "$TESTID" "$(date -u +%H:%M:%SZ)" >> "$LAST_TEST_STATE"
                printf '%s\n' "${PREFIX}${RED}⚠${NC} ${line}"
                ;;
            *)
                [ -n "$line" ] && printf '%s\n' "$line"
                ;;
        esac
        if [ -n "$TESTID" ]; then
            _cnt=$(cat "$TMP_SM_DONE" 2>/dev/null || echo 0)
            echo $(( _cnt + 1 )) > "$TMP_SM_DONE"
            printf '%s [%s]' "$TESTID" "$_TIME" > "$TMP_SM_LAST"
        fi
    done

    # Stop stress monitor
    if [ -n "$_rs_sm_pid" ]; then
        kill "$_rs_sm_pid" 2>/dev/null
        wait "$_rs_sm_pid" 2>/dev/null || true
        _STRESS_MON_PID=""
    fi

    # Append kyua verbose report for this suite (while kyua.db is fresh)
    {
        printf '\n=================================================================\n'
        printf 'SUITE: %s — DETAILED RESULTS\n' "$_rs_suite"
        printf '=================================================================\n'
    } >> "$REPORT_TXT"
    kyua report --verbose --results-filter passed,skipped,xfail,broken,failed \
        >> "$REPORT_TXT" 2>&1

    # Per-suite JUnit XML
    kyua report-junit > "$RESULTS_DIR/report-${_rs_suite}.xml" 2>/dev/null || true

    # Clean up per-suite filtered kyuafile if we created one
    [ -n "$_rs_kf" ] && rm -f "$_rs_kf"

    _rs_ec=$(cat "$_rs_exit_f" 2>/dev/null || echo 1)
    rm -f "$_rs_exit_f"
    return "$_rs_ec"
}

# ── Stress batch helpers ────────────────────────────────────────────────────

# _suite_has_batch_tests SUITE CATEGORIES
# Returns 0 if the filtered Kyuafile for SUITE with CATEGORIES has >=1 test.
# Returns 1 (skip silently) otherwise.
_suite_has_batch_tests() {
    _sht_suite="$1"
    _sht_cats="$2"
    _sht_orig_cats="$CATEGORIES"
    CATEGORIES="$_sht_cats"
    _sht_dir=$(suite_install_dir "$_sht_suite")
    if [ -z "$_sht_dir" ]; then
        CATEGORIES="$_sht_orig_cats"
        return 1
    fi
    if [ ! -d "$_sht_dir" ] || [ ! -f "$_sht_dir/Kyuafile" ]; then
        CATEGORIES="$_sht_orig_cats"
        return 1
    fi
    _sht_kf=$(build_filtered_kyuafile "$_sht_dir")
    _sht_count=$(grep -c '^atf_test_program' "$_sht_kf" 2>/dev/null)
    _sht_count=${_sht_count:-0}
    # Clean up filtered Kyuafile if one was created (lives in install dir)
    case "$_sht_kf" in *".kyuafile_filtered_"*) rm -f "$_sht_kf" ;; esac
    CATEGORIES="$_sht_orig_cats"
    [ "${_sht_count}" -gt 0 ] 2>/dev/null || return 1
}

# _run_stress_batches
# Runs stress tests in 4 sequential resource-based batches.
# Within each batch all matching tests run in parallel ($PARALLELISM).
# Batches that have no tests for the active SUITE_LIST are skipped silently.
# Globals written: same as _run_suite_once (TMP_PASS, TMP_FAIL, etc., REPORT_TXT)
# Globals read:    SUITE_LIST, PARALLELISM, REPORT_TXT, TEST_EXIT_CODE
_effective_batch_categories() {
    _ebc_batch="$1"
    _ebc_orig="$2"

    if [ -n "$_ebc_orig" ]; then
        category_intersection "$_ebc_orig" "$_ebc_batch"
    else
        printf '%s' "$_ebc_batch"
    fi
}

_run_stress_batches() {
    _srb_orig_cats="$CATEGORIES"

    # Which suites can carry stress tests
    _srb_ibs_active=0; _srb_str_active=0; _srb_tsc_active=0
    for _srb_s in $SUITE_LIST; do
        case "$_srb_s" in IBS|ALL)    _srb_ibs_active=1 ;; esac
        case "$_srb_s" in STRESS|ALL) _srb_str_active=1 ;; esac
        case "$_srb_s" in TSC|ALL)    _srb_tsc_active=1 ;; esac
    done

    # ── Batch 1 — CPU stress ─────────────────────────────────────────────
    # Tests: cpu_stress_test, ibs_op_stress_test, ibs_fetch_stress_test (STRESS suite)
    #        ibs_stress_test, ibs_cpu_stress_test (TC-STR, IBS suite)
    #        ibs_nmi_stress_test (TC-NMISTR, IBS suite)
    #        tsc_stress_test (TC-TSCSTR, TSC suite)
    _srb_b1_cats="TC-CSTR TC-ISTR TC-FSTR TC-STR TC-NMISTR TC-TSCSTR"
    _srb_b1_eff=$(_effective_batch_categories "$_srb_b1_cats" "$_srb_orig_cats")
    _srb_b1_run=0
    {
        printf '\n=================================================================\n'
        printf 'STRESS BATCH 1/4 — CPU  [%s]\n' "$_srb_b1_eff"
        printf '=================================================================\n'
    } >> "$REPORT_TXT"
    if [ -n "$_srb_b1_eff" ] && [ "$_srb_str_active" -eq 1 ] && _suite_has_batch_tests STRESS "$_srb_b1_eff"; then
        log_info "Stress Batch 1/4 — CPU: STRESS suite"
        CATEGORIES="$_srb_b1_eff"
        _run_suite_once STRESS || TEST_EXIT_CODE=1
        _srb_b1_run=1
    fi
    if [ -n "$_srb_b1_eff" ] && [ "$_srb_ibs_active" -eq 1 ] && _suite_has_batch_tests IBS "$_srb_b1_eff"; then
        log_info "Stress Batch 1/4 — CPU: IBS suite"
        CATEGORIES="$_srb_b1_eff"
        _run_suite_once IBS || TEST_EXIT_CODE=1
        _srb_b1_run=1
    fi
    if [ -n "$_srb_b1_eff" ] && [ "$_srb_tsc_active" -eq 1 ] && _suite_has_batch_tests TSC "$_srb_b1_eff"; then
        log_info "Stress Batch 1/4 — CPU: TSC suite"
        CATEGORIES="$_srb_b1_eff"
        _run_suite_once TSC || TEST_EXIT_CODE=1
        _srb_b1_run=1
    fi
    [ "$_srb_b1_run" -eq 0 ] && printf '  (no tests for active suite selection)\n' >> "$REPORT_TXT"

    # ── Batch 2 — Memory stress ──────────────────────────────────────────
    # Tests: mem_stress_test (STRESS suite)
    #        ibs_mem_stress_test (TC-MEMIBS, IBS suite)
    _srb_b2_cats="TC-MSTR TC-MEMIBS"
    _srb_b2_eff=$(_effective_batch_categories "$_srb_b2_cats" "$_srb_orig_cats")
    _srb_b2_run=0
    {
        printf '\n=================================================================\n'
        printf 'STRESS BATCH 2/4 — Memory  [%s]\n' "$_srb_b2_eff"
        printf '=================================================================\n'
    } >> "$REPORT_TXT"
    if [ -n "$_srb_b2_eff" ] && [ "$_srb_str_active" -eq 1 ] && _suite_has_batch_tests STRESS "$_srb_b2_eff"; then
        log_info "Stress Batch 2/4 — Memory: STRESS suite"
        CATEGORIES="$_srb_b2_eff"
        _run_suite_once STRESS || TEST_EXIT_CODE=1
        _srb_b2_run=1
    fi
    if [ -n "$_srb_b2_eff" ] && [ "$_srb_ibs_active" -eq 1 ] && _suite_has_batch_tests IBS "$_srb_b2_eff"; then
        log_info "Stress Batch 2/4 — Memory: IBS suite"
        CATEGORIES="$_srb_b2_eff"
        _run_suite_once IBS || TEST_EXIT_CODE=1
        _srb_b2_run=1
    fi
    [ "$_srb_b2_run" -eq 0 ] && printf '  (no tests for active suite selection)\n' >> "$REPORT_TXT"

    # ── Batch 3 — Disk stress ────────────────────────────────────────────
    _srb_b3_cats="TC-DSTR"
    _srb_b3_eff=$(_effective_batch_categories "$_srb_b3_cats" "$_srb_orig_cats")
    {
        printf '\n=================================================================\n'
        printf 'STRESS BATCH 3/4 — Disk  [%s]\n' "$_srb_b3_eff"
        printf '=================================================================\n'
    } >> "$REPORT_TXT"
    if [ -n "$_srb_b3_eff" ] && [ "$_srb_str_active" -eq 1 ] && _suite_has_batch_tests STRESS "$_srb_b3_eff"; then
        log_info "Stress Batch 3/4 — Disk: STRESS suite"
        CATEGORIES="$_srb_b3_eff"
        _run_suite_once STRESS || TEST_EXIT_CODE=1
    else
        printf '  (no tests for active suite selection)\n' >> "$REPORT_TXT"
    fi

    # ── Batch 4 — Network stress ─────────────────────────────────────────
    _srb_b4_cats="TC-NSTR"
    _srb_b4_eff=$(_effective_batch_categories "$_srb_b4_cats" "$_srb_orig_cats")
    {
        printf '\n=================================================================\n'
        printf 'STRESS BATCH 4/4 — Network  [%s]\n' "$_srb_b4_eff"
        printf '=================================================================\n'
    } >> "$REPORT_TXT"
    if [ -n "$_srb_b4_eff" ] && [ "$_srb_str_active" -eq 1 ] && _suite_has_batch_tests STRESS "$_srb_b4_eff"; then
        log_info "Stress Batch 4/4 — Network: STRESS suite"
        CATEGORIES="$_srb_b4_eff"
        _run_suite_once STRESS || TEST_EXIT_CODE=1
    else
        printf '  (no tests for active suite selection)\n' >> "$REPORT_TXT"
    fi

    CATEGORIES="$_srb_orig_cats"
}

# Test execution and reporting
run_all_tests() {
    log_info "Running suites: ${SUITE_LIST}"

    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would run suites: $SUITE_LIST (dry run)"
        for _suite in $SUITE_LIST; do
            log_info "Would validate and run: $_suite"
        done
        [ "$WITH_STRESS" -eq 1 ] && log_info "Would start background stressors"
        return 0
    fi

    preflight_checks
    load_module

    if [ $DRY_RUN -eq 0 ]; then
        for _suite in $SUITE_LIST; do
            validate_installed_suite "$_suite" || return 1
        done
        [ "$WITH_STRESS" -eq 1 ] && start_background_stressors

        print_cpu_test_context

        mkdir -p "$RESULTS_DIR"
        REPORT_TXT="$RESULTS_DIR/report.txt"
        REPORT_XML="$RESULTS_DIR/report.xml"

        # ── Counter temp files — initialised once, accumulated across all suites ──
        TMP_PASS="/tmp/ibs_pass_$$.tmp"
        TMP_FAIL="/tmp/ibs_fail_$$.tmp"
        TMP_SKIP="/tmp/ibs_skip_$$.tmp"
        TMP_XFAIL="/tmp/ibs_xfail_$$.tmp"
        TMP_BROKEN="/tmp/ibs_broken_$$.tmp"
        TMP_CRIT_F="/tmp/ibs_crit_f_$$.tmp"
        TMP_HIGH_P="/tmp/ibs_high_p_$$.tmp"
        TMP_HIGH_F="/tmp/ibs_high_f_$$.tmp"
        TMP_MED_P="/tmp/ibs_med_p_$$.tmp"
        TMP_MED_F="/tmp/ibs_med_f_$$.tmp"
        TMP_MATRIX="/tmp/ibs_matrix_$$.tmp"
        TMP_DESC="/tmp/ibs_desc_$$.tmp"
        TMP_SM_DONE="/tmp/ibs_sm_done_$$.tmp"
        TMP_SM_LAST="/tmp/ibs_sm_last_$$.tmp"
        for _f in "$TMP_PASS" "$TMP_FAIL" "$TMP_SKIP" "$TMP_XFAIL" "$TMP_BROKEN" \
                  "$TMP_CRIT_F" "$TMP_HIGH_P" "$TMP_HIGH_F" "$TMP_MED_P" "$TMP_MED_F"; do
            echo 0 > "$_f"
        done
        > "$TMP_MATRIX"
        > "$TMP_DESC"
        echo 0 > "$TMP_SM_DONE"
        printf '(starting)' > "$TMP_SM_LAST"

        # ── Report header ──
        {
            printf "AMD CI Test Suite — Comprehensive Report\n"
            printf "Generated  : %s\n" "$(date)"
            printf "System     : %s\n" "$(uname -a)"
            printf "Suites     : %s\n" "$SUITE_LIST"
            printf "Parallelism: %s\n" "$PARALLELISM"
            [ "$WITH_STRESS" -eq 1 ] && \
                printf "Background : stress active (cpu_stressor/mem_stressor/net_stressor/disk_stressor)\n"
            printf "=================================================================\n\n"
        } > "$REPORT_TXT"

        # ── Live-log header (panic recovery) ──
        # Rotate previous log/state before overwriting so post-reboot 'r' can read them.
        [ -f "$LAST_RUN_LOG"    ] && cp "$LAST_RUN_LOG"    "${LAST_RUN_LOG}.prev"
        [ -f "$LAST_TEST_STATE" ] && cp "$LAST_TEST_STATE" "${LAST_TEST_STATE}.prev"
        {
            printf '=== ibs-ci run started: %s ===\n' "$(date)"
            printf 'SUITES=%s  WITH_STRESS=%s\n\n' "$SUITE_LIST" "$WITH_STRESS"
        } > "$LAST_RUN_LOG"
        {
            printf 'SUITES=%s\n' "$SUITE_LIST"
            printf 'STARTED=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
            printf 'RESULTS_DIR=%s\n' "$RESULTS_DIR"
            printf 'WITH_STRESS=%s\n' "$WITH_STRESS"
            printf 'LAST_COMPLETED=\nLAST_STATUS=\nLAST_SEEN_AT=\n'
        } > "$LAST_TEST_STATE"

        # ── Two-phase execution ──────────────────────────────────────────────
        # Phase 1: non-stress tests run with suite-safe parallelism; L3 PMU
        # runtime tests are serialized inside _run_suite_once().
        # Phase 2: stress tests run in 4 sequential resource-based batches
        #          (CPU → Memory → Disk → Network); tests within each batch
        #          run in parallel with each other at $PARALLELISM.
        TEST_EXIT_CODE=0
        _ph_orig_cats="$CATEGORIES"

        # Non-stress categories for IBS, UMCDF, PMC, TSC, and L3 suites.
        # TC-NMISTR and TC-MEMIBS are stress; TC-STR is stress — all excluded here.
        _NONSTRESS_CATS="TC-DET TC-MSR TC-INT TC-DATA TC-SMP TC-HWPMC TC-DRV \
TC-CONC TC-SEC TC-API TC-UNIT TC-UMCDET TC-UMCPMC TC-UMCUNIT \
TC-PMCAPI TC-PMCSTAT TC-TSC-DET TC-TSC-DRF TC-TSC-INV TC-DET-L3 TC-UNC-L3"
        if [ -n "$_ph_orig_cats" ]; then
            _ph_cats=$(category_intersection \
                "$_ph_orig_cats" "$_NONSTRESS_CATS")
        else
            _ph_cats="$_NONSTRESS_CATS"
        fi

        # ── Phase 1: non-stress tests ──
        {
            printf '\n=================================================================\n'
            printf 'PHASE 1 — NON-STRESS TESTS  (parallelism: %s; L3 serial)\n' "$PARALLELISM"
            printf '=================================================================\n'
        } >> "$REPORT_TXT"
        log_info "Phase 1/2: non-stress tests (parallelism: $PARALLELISM; L3 serial)"
        for _s in $SUITE_LIST; do
            case "$_s" in
                STRESS) continue ;;  # STRESS suite contains only stress tests
            esac
            if [ -n "$_ph_cats" ] && _suite_has_batch_tests "$_s" "$_ph_cats"; then
                CATEGORIES="$_ph_cats"
                _run_suite_once "$_s" || TEST_EXIT_CODE=1
            fi
        done
        CATEGORIES="$_ph_orig_cats"

        # ── Phase 2: stress batches ──
        # Check whether any active suite has stress tests.
        _ph_has_stress=0
        for _s in $SUITE_LIST; do
            case "$_s" in IBS|TSC|STRESS|ALL) _ph_has_stress=1 ;; esac
        done
        if [ "$_ph_has_stress" -eq 1 ]; then
            {
                printf '\n=================================================================\n'
                printf 'PHASE 2 — STRESS BATCHES  (parallelism: %s per batch)\n' "$PARALLELISM"
                printf '=================================================================\n'
            } >> "$REPORT_TXT"
            log_info "Phase 2/2: stress batches (parallelism: $PARALLELISM per batch)"
            _run_stress_batches
        fi
        CATEGORIES="$_ph_orig_cats"

        [ "$WITH_STRESS" -eq 1 ] && stop_background_stressors
        printf '\n=== ibs-ci run finished: %s ===\n' "$(date)" >> "$LAST_RUN_LOG"
        rm -f "$TMP_SM_DONE" "$TMP_SM_LAST"

        # ── Read accumulated counters ──
        PASSED_TESTS=$(read_counter_file "$TMP_PASS")
        FAILED_TESTS=$(read_counter_file "$TMP_FAIL")
        SKIPPED_TESTS=$(read_counter_file "$TMP_SKIP")
        XFAIL_TESTS=$(read_counter_file "$TMP_XFAIL")
        BROKEN_TESTS=$(read_counter_file "$TMP_BROKEN")
        CRIT_FAIL=$(read_counter_file "$TMP_CRIT_F")
        HIGH_PASS=$(read_counter_file "$TMP_HIGH_P")
        HIGH_FAIL=$(read_counter_file "$TMP_HIGH_F")
        MED_PASS=$(read_counter_file "$TMP_MED_P")
        MED_FAIL=$(read_counter_file "$TMP_MED_F")
        TOTAL_TESTS=$((PASSED_TESTS + FAILED_TESTS + SKIPPED_TESTS + XFAIL_TESTS + BROKEN_TESTS))
        rm -f "$TMP_PASS" "$TMP_FAIL" "$TMP_SKIP" "$TMP_XFAIL" "$TMP_BROKEN" \
              "$TMP_CRIT_F" "$TMP_HIGH_P" "$TMP_HIGH_F" "$TMP_MED_P" "$TMP_MED_F"

        if [ "$TOTAL_TESTS" -gt 0 ]; then
            PASSED_PERCENT=$((PASSED_TESTS * 100 / TOTAL_TESTS))
            FAILED_PERCENT=$((FAILED_TESTS * 100 / TOTAL_TESTS))
            SKIPPED_PERCENT=$((SKIPPED_TESTS * 100 / TOTAL_TESTS))
        else
            PASSED_PERCENT=0; FAILED_PERCENT=0; SKIPPED_PERCENT=0
        fi

        HIGH_TOTAL=$((HIGH_PASS + HIGH_FAIL))
        MED_TOTAL=$((MED_PASS + MED_FAIL))
        if [ "$HIGH_TOTAL" -gt 0 ]; then
            HIGH_PASS_PCT=$((HIGH_PASS * 100 / HIGH_TOTAL))
        else
            HIGH_PASS_PCT=-1
        fi
        if [ "$MED_TOTAL" -gt 0 ]; then
            MED_PASS_PCT=$((MED_PASS * 100 / MED_TOTAL))
        else
            MED_PASS_PCT=-1
        fi

        # ── Console summary ──
        echo ""
        echo "================================================================="
        echo "SUMMARY:"
        printf "  Total:   %3d   Passed: %3d (%d%%)  Failed: %3d (%d%%)  Skipped: %3d (%d%%)\n" \
            "$TOTAL_TESTS" "$PASSED_TESTS" "$PASSED_PERCENT" \
            "$FAILED_TESTS" "$FAILED_PERCENT" "$SKIPPED_TESTS" "$SKIPPED_PERCENT"
        printf "           Exp.Failures: %3d   Broken: %3d\n" "$XFAIL_TESTS" "$BROKEN_TESTS"
        echo ""
        echo "SEVERITY CRITERIA:"
        if [ "$CRIT_FAIL" -eq 0 ]; then
            printf "  ${RED}[CRITICAL]${NC}  %d failed    ${GREEN}PASS${NC}\n" "$CRIT_FAIL"
        else
            printf "  ${RED}[CRITICAL]${NC}  %d failed    ${RED}FAIL${NC}\n" "$CRIT_FAIL"
        fi
        if [ "$HIGH_PASS_PCT" -lt 0 ]; then
            printf "  ${YELLOW}[HIGH]${NC}      N/A (no HIGH tests ran)\n"
        elif [ "$HIGH_PASS_PCT" -ge 95 ]; then
            printf "  ${YELLOW}[HIGH]${NC}      %d%% pass rate  ${GREEN}PASS${NC}  (>= 95%% required)\n" "$HIGH_PASS_PCT"
        else
            printf "  ${YELLOW}[HIGH]${NC}      %d%% pass rate  ${RED}FAIL${NC}  (>= 95%% required)\n" "$HIGH_PASS_PCT"
        fi
        if [ "$MED_PASS_PCT" -lt 0 ]; then
            printf "  ${CYAN}[MEDIUM]${NC}    N/A (no MEDIUM tests ran)\n"
        elif [ "$MED_PASS_PCT" -ge 80 ]; then
            printf "  ${CYAN}[MEDIUM]${NC}    %d%% pass rate  ${GREEN}PASS${NC}  (>= 80%% required)\n" "$MED_PASS_PCT"
        else
            printf "  ${CYAN}[MEDIUM]${NC}    %d%% pass rate  ${RED}FAIL${NC}  (>= 80%% required)\n" "$MED_PASS_PCT"
        fi
        echo "================================================================="

        # ── Verdict ──
        VERDICT_FAIL=0
        [ "$CRIT_FAIL" -gt 0 ]                                     && VERDICT_FAIL=1
        [ "$HIGH_PASS_PCT" -ge 0 ] && [ "$HIGH_PASS_PCT" -lt 95 ]  && VERDICT_FAIL=1

        if [ "$VERDICT_FAIL" -eq 0 ]; then
            if [ "$MED_PASS_PCT" -lt 0 ] || [ "$MED_PASS_PCT" -ge 80 ]; then
                log_success "VERDICT: PASS -all criteria met"
            else
                log_warning "VERDICT: CONDITIONAL -CRITICAL/HIGH passed; MEDIUM below 80%"
            fi
        else
            log_error "VERDICT: FAIL"
            [ "$CRIT_FAIL" -gt 0 ]                                    && log_error "  $CRIT_FAIL CRITICAL test(s) failed"
            [ "$HIGH_PASS_PCT" -ge 0 ] && [ "$HIGH_PASS_PCT" -lt 95 ] && log_error "  HIGH pass rate ${HIGH_PASS_PCT}% is below the 95% threshold"
        fi

        log_info "Generating reports..."

        # ── Append summary to REPORT_TXT ──
        {
            printf "\n=================================================================\n"
            printf "TEST RUN SUMMARY\n"
            printf "=================================================================\n"
            printf "Tests Run : %d\n" "$TOTAL_TESTS"
            printf "Passed    : %d (%d%%)\n" "$PASSED_TESTS" "$PASSED_PERCENT"
            printf "Failed    : %d (%d%%)\n" "$FAILED_TESTS" "$FAILED_PERCENT"
            printf "Skipped   : %d (%d%%)\n" "$SKIPPED_TESTS" "$SKIPPED_PERCENT"
            printf "Exp.Fail  : %d\n" "$XFAIL_TESTS"
            printf "Broken    : %d\n" "$BROKEN_TESTS"
            printf "\nSEVERITY CRITERIA:\n"
            if [ "$CRIT_FAIL" -eq 0 ]; then
                printf "  [CRITICAL]  %d failed    PASS\n" "$CRIT_FAIL"
            else
                printf "  [CRITICAL]  %d failed    FAIL\n" "$CRIT_FAIL"
            fi
            if [ "$HIGH_PASS_PCT" -lt 0 ]; then
                printf "  [HIGH]      N/A (no HIGH tests ran)\n"
            elif [ "$HIGH_PASS_PCT" -ge 95 ]; then
                printf "  [HIGH]      %d%% pass rate  PASS  (>= 95%% required)\n" "$HIGH_PASS_PCT"
            else
                printf "  [HIGH]      %d%% pass rate  FAIL  (>= 95%% required)\n" "$HIGH_PASS_PCT"
            fi
            if [ "$MED_PASS_PCT" -lt 0 ]; then
                printf "  [MEDIUM]    N/A (no MEDIUM tests ran)\n"
            elif [ "$MED_PASS_PCT" -ge 80 ]; then
                printf "  [MEDIUM]    %d%% pass rate  PASS  (>= 80%% required)\n" "$MED_PASS_PCT"
            else
                printf "  [MEDIUM]    %d%% pass rate  FAIL  (>= 80%% required)\n" "$MED_PASS_PCT"
            fi
            printf "\n"
            if [ "$VERDICT_FAIL" -eq 0 ]; then
                if [ "$MED_PASS_PCT" -lt 0 ] || [ "$MED_PASS_PCT" -ge 80 ]; then
                    printf "VERDICT: APPROVED -all criteria met\n"
                else
                    printf "VERDICT: CONDITIONAL -CRITICAL/HIGH passed; MEDIUM below 80%%\n"
                fi
            else
                printf "VERDICT: NOT APPROVED\n"
                [ "$CRIT_FAIL" -gt 0 ]                                    && printf "  Reason: %d CRITICAL test(s) failed\n" "$CRIT_FAIL"
                [ "$HIGH_PASS_PCT" -ge 0 ] && [ "$HIGH_PASS_PCT" -lt 95 ] && printf "  Reason: HIGH pass rate %d%% below the 95%% threshold\n" "$HIGH_PASS_PCT"
            fi
            printf "=================================================================\n"
        } >> "$REPORT_TXT"

        # ── Merge per-suite JUnit XMLs into one report.xml ──
        {
            printf '<?xml version="1.0" encoding="UTF-8"?>\n<testsuites>\n'
            for _s in $SUITE_LIST; do
                _xf="$RESULTS_DIR/report-${_s}.xml"
                if [ -f "$_xf" ]; then
                    grep -v '<?xml\|<testsuites\|</testsuites>' "$_xf" || true
                fi
            done
            printf '</testsuites>\n'
        } > "$REPORT_XML"

        log_success "Reports saved to: $RESULTS_DIR"

        # ── DIAGNOSTIC SUMMARY section appended to report.txt ──────────────
        # For every non-passed test, write a structured diagnostic block with
        # Operation / Root cause / Action fields drawn from diag_for_test().
        _ds_total=$((FAILED_TESTS + BROKEN_TESTS + SKIPPED_TESTS))
        if [ "$_ds_total" -gt 0 ]; then
            {
                printf '\n=================================================================\n'
                printf 'DIAGNOSTIC SUMMARY — NON-PASSED TESTS\n'
                printf '=================================================================\n'
                printf 'Legend: [FAIL/BRKN] requires investigation\n'
                printf '        [SKIP]      actionable — missing feature/patch/config\n'
                printf '        [SKIP-OK]   expected   — hardware or version mismatch\n'
                printf '=================================================================\n'
            } >> "$REPORT_TXT"

            # FAILED and BROKEN
            if [ "$((FAILED_TESTS + BROKEN_TESTS))" -gt 0 ]; then
                grep -E "^[a-z_].*:.*->  (failed|broken):" "$REPORT_TXT" | \
                while IFS= read -r _dl; do
                    _tid=$(printf '%s' "$_dl" | sed 's/  ->  .*//')
                    _res=$(printf '%s' "$_dl" | sed 's/.*->  //' | sed 's/ \[.*//')
                    _diag=$(diag_for_test "$_tid")
                    _op=$(printf '%s'     "$_diag" | grep '^OPERATION:' | sed 's/^OPERATION://')
                    _cause=$(printf '%s'  "$_diag" | grep '^CAUSE:'     | sed 's/^CAUSE://')
                    _action=$(printf '%s' "$_diag" | grep '^ACTION:'    | sed 's/^ACTION://')
                    printf '\n----------------------------------------------------------------\n'
                    printf '[FAIL/BRKN] %s\n' "$_tid"
                    printf '  Result    : %s\n' "$_res"
                    [ -n "$_op"     ] && printf '  Operation : %s\n' "$_op"
                    [ -n "$_cause"  ] && printf '  Root cause: %s\n' "$_cause"
                    [ -n "$_action" ] && printf '  Action    : %s\n' "$_action"
                done >> "$REPORT_TXT"
            fi

            # SKIPPED
            if [ "$SKIPPED_TESTS" -gt 0 ]; then
                grep -E "^[a-z_].*:.*->  skipped:" "$REPORT_TXT" | \
                while IFS= read -r _dl; do
                    _tid=$(printf '%s' "$_dl" | sed 's/  ->  skipped:.*//')
                    _reason=$(printf '%s' "$_dl" | sed 's/.*->  skipped: //' | sed 's/ \[.*//')
                    _diag=$(diag_for_test "$_tid")
                    _class=$(printf '%s'  "$_diag" | grep '^CLASS:'     | sed 's/^CLASS://')
                    _op=$(printf '%s'     "$_diag" | grep '^OPERATION:' | sed 's/^OPERATION://')
                    _cause=$(printf '%s'  "$_diag" | grep '^CAUSE:'     | sed 's/^CAUSE://')
                    _action=$(printf '%s' "$_diag" | grep '^ACTION:'    | sed 's/^ACTION://')
                    if [ "$_class" = "normal" ]; then
                        printf '[SKIP-OK]   %-52s  %s\n' "$_tid" "$_reason"
                    else
                        printf '\n----------------------------------------------------------------\n'
                        printf '[SKIP]      %s\n' "$_tid"
                        printf '  Skip msg  : %s\n' "$_reason"
                        [ -n "$_op"     ] && printf '  Operation : %s\n' "$_op"
                        [ -n "$_cause"  ] && printf '  Root cause: %s\n' "$_cause"
                        [ -n "$_action" ] && printf '  Action    : %s\n' "$_action"
                    fi
                done >> "$REPORT_TXT"
            fi

            printf '\n=================================================================\n' >> "$REPORT_TXT"
        fi

        # ── Failures & broken tests detail (from REPORT_TXT) ──────────────
        if [ "$FAILED_TESTS" -gt 0 ] || [ "$BROKEN_TESTS" -gt 0 ]; then
            echo ""
            echo "── Failures & broken tests ──"
            echo ""
            # Extract per-test blocks from the kyua verbose report, then augment
            # each FAIL/BROKEN entry with full stdout/stderr and diagnostic info.
            awk -v RED="$RED" -v BOLD="$BOLD" -v DIM="$DIM" -v RESET="$NC" \
                -v CYAN="$CYAN" -v YELLOW="$YELLOW" \
            '
            function flush_block(    tag, i) {
                if (!in_fail || testid == "") return
                tag = (result ~ /^broken/) ? "BRKN" : "FAIL"
                printf "  %s━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━%s\n", BOLD, RESET
                printf "  [%s%s%s%s] %s%s%s\n", RED, BOLD, tag, RESET, BOLD, testid, RESET
                printf "  %s  Result  :%s %s%s%s\n", DIM, RESET, RED, result, RESET
                if (n_stdout > 0) {
                    printf "  %s  Stdout  :%s\n", DIM, RESET
                    for (i = 1; i <= n_stdout; i++)
                        printf "             %s%s%s\n", DIM, stdout_lines[i], RESET
                }
                if (n_stderr > 0) {
                    printf "  %s  Stderr  :%s\n", DIM, RESET
                    for (i = 1; i <= n_stderr; i++)
                        printf "             %s%s%s\n", RED, stderr_lines[i], RESET
                }
                printf "\n"
            }
            /^===> / {
                flush_block()
                in_fail = 0; in_stdout = 0; in_stderr = 0
                n_stdout = 0; n_stderr = 0
                delete stdout_lines; delete stderr_lines
                testid = substr($0, 6)
                result = ""
                if (testid ~ /^(Execution context|Failed tests|Skipped tests|Summary|Passed tests|Expected failures|Broken tests)$/) testid = ""
                next
            }
            testid == "" { next }
            /^Result:/ {
                result = $0; sub(/^Result:[ \t]+/, "", result)
                if (result ~ /^failed|^broken/) in_fail = 1
                next
            }
            in_fail && /^Standard output:$/ { in_stdout = 1; in_stderr = 0; next }
            in_fail && /^Standard error:$/  { in_stderr = 1; in_stdout = 0; next }
            in_fail && in_stdout { stdout_lines[++n_stdout] = $0 }
            in_fail && in_stderr { stderr_lines[++n_stderr] = $0 }
            END { flush_block() }
            ' "$REPORT_TXT" | while IFS= read -r _fline; do
                # After awk emits each block, inject the diag fields.
                # We use a sentinel approach: re-read the test ID from the [FAIL]/[BRKN] line.
                _ftid=""
                if printf '%s' "$_fline" | grep -qE '^\s+\[(FAIL|BRKN)\]'; then
                    _ftid=$(printf '%s' "$_fline" | sed 's/.*\(FAIL\|BRKN\)\] //' | \
                            sed "s/$(printf '\033')\\[[0-9;]*m//g")
                    printf '%s\n' "$_fline"
                    # Emit diagnostic fields immediately after the test ID line
                    _diag=$(diag_for_test "$_ftid")
                    _op=$(printf '%s' "$_diag"    | grep '^OPERATION:' | sed 's/^OPERATION://')
                    _cause=$(printf '%s' "$_diag" | grep '^CAUSE:'     | sed 's/^CAUSE://')
                    _action=$(printf '%s' "$_diag"| grep '^ACTION:'    | sed 's/^ACTION://')
                    [ -n "$_op"     ] && printf "  %s  Operation:%s %s\n" "$CYAN" "$NC" "$_op"
                    [ -n "$_cause"  ] && printf "  %s  Root cause:%s %s\n" "$YELLOW" "$NC" "$_cause"
                    [ -n "$_action" ] && printf "  %s  Action    :%s %s\n" "$BOLD" "$NC" "$_action"
                else
                    printf '%s\n' "$_fline"
                fi
            done
        fi

        # ── Skipped tests ──────────────────────────────────────────────────
        if [ "$SKIPPED_TESTS" -gt 0 ]; then
            echo ""
            echo "── Skipped tests ──"
            echo ""
            grep -E "^[a-z_].*:.*->  skipped:" "$REPORT_TXT" | while IFS= read -r line; do
                TESTID=$(printf '%s' "$line" | sed 's/  ->  skipped:.*//')
                REASON=$(printf '%s' "$line" | sed 's/.*->  skipped: //' | sed 's/ \[.*//')
                _diag=$(diag_for_test "$TESTID")
                _class=$(printf '%s' "$_diag"  | grep '^CLASS:'     | sed 's/^CLASS://')
                _op=$(printf '%s' "$_diag"     | grep '^OPERATION:' | sed 's/^OPERATION://')
                _cause=$(printf '%s' "$_diag"  | grep '^CAUSE:'     | sed 's/^CAUSE://')
                _action=$(printf '%s' "$_diag" | grep '^ACTION:'    | sed 's/^ACTION://')
                if [ "$_class" = "normal" ]; then
                    # Expected hardware/version mismatch — compact one-liner
                    printf "  [${YELLOW}SKIP-OK${NC}] ${DIM}%-52s${NC}  %s\n" \
                        "$TESTID" "$REASON"
                else
                    # Actionable skip — full diagnostic block
                    printf "  ${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
                    printf "  [${YELLOW}SKIP${NC}] ${BOLD}%s${NC}\n" "$TESTID"
                    printf "  ${DIM}  Skip msg  :${NC} %s\n" "$REASON"
                    [ -n "$_op"     ] && printf "  ${CYAN}  Operation :${NC} %s\n" "$_op"
                    [ -n "$_cause"  ] && printf "  ${YELLOW}  Root cause:${NC} %s\n" "$_cause"
                    [ -n "$_action" ] && printf "  ${BOLD}  Action    :${NC} %s\n" "$_action"
                    echo ""
                fi
            done
        fi

        if [ "$PASSED_TESTS" -gt 0 ]; then
            echo ""
            echo "── Passed tests ──"
            echo ""
            grep -E "^[a-z_].*:.*->  passed" "$REPORT_TXT" | while IFS= read -r line; do
                TESTID=$(printf '%s' "$line" | sed 's/  ->  passed.*//')
                TIMING=$(printf '%s' "$line" | grep -oE '\[[0-9]+\.[0-9]+s\]' || printf '')
                printf "  [${GREEN}PASS${NC}] ${BOLD}%-48s${NC}  ${DIM}%s${NC}\n" \
                    "$TESTID" "$TIMING"
            done
        fi

        # ── Results matrix ──
        echo ""
        echo "===================================================================================================="
        echo "TEST RESULTS MATRIX"
        echo "===================================================================================================="
        printf "  %-76s %-11s %-9s %-8s %s\n" \
            "TEST CASE" "CAT" "SEV" "STATUS" "TIME"
        printf "  %-76s %-11s %-9s %-8s %s\n" \
            "----------------------------------------------------------------------------" "-----------" "---------" "--------" "------"
        while IFS='|' read -r _tc _cat _sev _st _tm; do
            case "$_st" in
                passed)  _scol="$GREEN";  _stxt="PASS"  ;;
                failed)  _scol="$RED";    _stxt="FAIL"  ;;
                skipped) _scol="$YELLOW"; _stxt="SKIP"  ;;
                xfail)   _scol="$CYAN";   _stxt="XFAIL" ;;
                broken)  _scol="$RED";    _stxt="BRKN"  ;;
                *)       _scol="$WHITE";  _stxt="$_st"  ;;
            esac
            case "$_sev" in
                CRITICAL) _sevcol="$RED" ;;
                HIGH)     _sevcol="$YELLOW" ;;
                MEDIUM)   _sevcol="$CYAN" ;;
                *)        _sevcol="$WHITE" ;;
            esac
            printf "  %-76s %-11s ${_sevcol}%-9s${NC} ${_scol}%-8s${NC} %s\n" \
                "$_tc" "$_cat" "$_sev" "$_stxt" "$_tm"
        done < "$TMP_MATRIX"
        printf "  %-76s %-11s %-9s %-8s\n" \
            "----------------------------------------------------------------------------" "-----------" "---------" "--------"
        echo ""
        printf "  Total: %d   ${GREEN}Pass: %d (%d%%)${NC}" \
            "$TOTAL_TESTS" "$PASSED_TESTS" "$PASSED_PERCENT"
        if [ "$FAILED_TESTS" -gt 0 ]; then
            printf "   ${RED}Fail: %d${NC}" "$FAILED_TESTS"
        else
            printf "   Fail: %d" "$FAILED_TESTS"
        fi
        printf "   Skip: %d   XFail: %d   Brkn: %d\n" \
            "$SKIPPED_TESTS" "$XFAIL_TESTS" "$BROKEN_TESTS"
        echo ""
        _denom=$((PASSED_TESTS + FAILED_TESTS + BROKEN_TESTS))
        if [ "$_denom" -gt 0 ]; then
            _rate=$(( PASSED_TESTS * 100 / _denom ))
            printf "  Pass rate (excl. skipped/xfail): ${BOLD}%d%%${NC}\n" "$_rate"
        else
            printf "  Pass rate: N/A\n"
        fi
        printf "  CRITICAL: %d failed   HIGH: " "$CRIT_FAIL"
        if [ "$HIGH_PASS_PCT" -lt 0 ]; then printf "N/A"; else printf "%d%%" "$HIGH_PASS_PCT"; fi
        printf "   MEDIUM: "
        if [ "$MED_PASS_PCT" -lt 0 ]; then printf "N/A"; else printf "%d%%" "$MED_PASS_PCT"; fi
        printf "\n\n"
        if [ "$VERDICT_FAIL" -eq 0 ]; then
            if [ "$MED_PASS_PCT" -lt 0 ] || [ "$MED_PASS_PCT" -ge 80 ]; then
                printf "  ${GREEN}${BOLD}VERDICT: APPROVED${NC}\n"
            else
                printf "  ${YELLOW}${BOLD}VERDICT: CONDITIONAL -CRITICAL/HIGH passed; MEDIUM below 80%%${NC}\n"
            fi
        else
            printf "  ${RED}${BOLD}VERDICT: NOT APPROVED${NC}\n"
            [ "$CRIT_FAIL" -gt 0 ] && \
                printf "  Reason: %d CRITICAL test(s) failed\n" "$CRIT_FAIL"
            [ "$HIGH_PASS_PCT" -ge 0 ] && [ "$HIGH_PASS_PCT" -lt 95 ] && \
                printf "  Reason: HIGH pass rate %d%% below the 95%% threshold\n" "$HIGH_PASS_PCT"
        fi
        echo "===================================================================================================="

        generate_html_report
        generate_html_index
        rm -f "$TMP_MATRIX" "$TMP_DESC"

        echo ""
        log_info "Full report : $REPORT_TXT"
        log_info "JUnit XML   : $REPORT_XML"
        log_info "Live log    : $LAST_RUN_LOG  (use --last-test after a panic)"

        if [ "$EMAIL_REQUESTED" -eq 1 ]; then
            _rt_verdict="UNKNOWN"
            grep -q "VERDICT: APPROVED"     "$REPORT_TXT" 2>/dev/null && _rt_verdict="APPROVED"
            grep -q "VERDICT: CONDITIONAL"  "$REPORT_TXT" 2>/dev/null && _rt_verdict="CONDITIONAL"
            grep -q "VERDICT: NOT APPROVED" "$REPORT_TXT" 2>/dev/null && _rt_verdict="NOT APPROVED"
            log_info "Sending report email to: $REPORT_EMAIL"
            send_report_email "$REPORT_TXT" "$_rt_verdict" "$REPORT_EMAIL"
        fi
    fi
}

run_specific_test() {
    TEST_NAME="$1"
    if [ "$SUITE" = "ALL" ]; then
        log_error "--run requires a concrete suite; use --suite IBS|UMCDF|PMC|TSC|L3|STRESS."
        return 1
    fi
    _rst_suite="$SUITE"
    case "$_rst_suite" in
        DEFAULT) _rst_suite="$_SUITE_PRIMARY" ;;
    esac
    TESTS_INSTALL_DIR=$(suite_install_dir "$_rst_suite")

    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would run $_rst_suite test $TEST_NAME (dry run)"
        return 0
    fi

    validate_installed_suite "$_rst_suite" || return 1

    log_info "Executing specific $_rst_suite test: $TEST_NAME"
    preflight_checks
    load_module

    cd "$TESTS_INSTALL_DIR" || return 1

    if [ $DRY_RUN -eq 0 ]; then
        _rst_list=$(kyua list 2>&1) || {
            log_error "[$_rst_suite] kyua list failed in $TESTS_INSTALL_DIR"
            printf '%s\n' "$_rst_list"
            return 1
        }
        if ! printf '%s\n' "$_rst_list" | awk -v t="$TEST_NAME" '
            BEGIN { found = 1 }
            $0 == t { found = 0 }
            index($0, t ":") == 1 { found = 0 }
            END { exit found }
        '; then
            log_error "[$_rst_suite] Test not found: $TEST_NAME"
            log_info "Available tests in $_rst_suite:"
            printf '%s\n' "$_rst_list" | sed 's/^/  /'
            return 1
        fi

        mkdir -p "$RESULTS_DIR"
        _rst_report_name=$(printf '%s' "$TEST_NAME" | tr '/:' '__')
        REPORT_TXT="$RESULTS_DIR/report-${_rst_report_name}.txt"

        confirm_cmd "Run single test in $TESTS_INSTALL_DIR" \
            "kyua test $TEST_NAME" || return 1
        log_verbose "Running kyua test $TEST_NAME..."

        {
            printf "%s Test Suite -Single Test Report\n" "$_rst_suite"
            printf "Test       : %s\n" "$TEST_NAME"
            printf "Generated  : %s\n" "$(date)"
            printf "System     : %s\n" "$(uname -a)"
            printf "=================================================================\n"
            printf "\n"
        } > "$REPORT_TXT"

        KYUA_OUT=$(kyua test "$TEST_NAME" 2>&1)
        _rst_kyua_rc=$?
        printf '%s\n' "$KYUA_OUT"
        printf '%s\n' "$KYUA_OUT" >> "$REPORT_TXT"

        T_PASS=$(printf '%s\n' "$KYUA_OUT" | grep -c " -> .*passed")
        T_FAIL=$(printf '%s\n' "$KYUA_OUT" | grep -c " -> .*failed")
        T_SKIP=$(printf '%s\n' "$KYUA_OUT" | grep -c " -> .*skipped")
        T_XFAIL=$(printf '%s\n' "$KYUA_OUT" | grep -c " -> .*expected_failure")
        T_BRKN=$(printf '%s\n' "$KYUA_OUT" | grep -c " -> .*broken")
        T_TOTAL=$((T_PASS + T_FAIL + T_SKIP + T_XFAIL + T_BRKN))

        if [ "$_rst_kyua_rc" -eq 0 ] && [ "$T_FAIL" -eq 0 ] && [ "$T_BRKN" -eq 0 ]; then
            SINGLE_VERDICT="APPROVED"
        else
            SINGLE_VERDICT="NOT APPROVED"
        fi

        {
            printf "\n"
            printf "=================================================================\n"
            printf "TEST RUN SUMMARY\n"
            printf "=================================================================\n"
            printf "Tests Run : %d\n" "$T_TOTAL"
            printf "Passed    : %d\n" "$T_PASS"
            printf "Failed    : %d\n" "$T_FAIL"
            printf "Skipped   : %d\n" "$T_SKIP"
            printf "Exp.Fail  : %d\n" "$T_XFAIL"
            printf "Broken    : %d\n" "$T_BRKN"
            printf "\n"
            printf "VERDICT: %s\n" "$SINGLE_VERDICT"
            printf "=================================================================\n"
        } >> "$REPORT_TXT"

        log_info "Report saved to: $REPORT_TXT"
        return "$_rst_kyua_rc"
    fi
}

_list_tests_for_suite() {
    _lts_suite="$1"
    _lts_dir="$2"

    if [ ! -d "$_lts_dir" ]; then
        log_error "[$_lts_suite] Tests not installed at $_lts_dir. Run --suite $_lts_suite --compile first."
        return 1
    fi
    if [ ! -f "$_lts_dir/Kyuafile" ]; then
        log_error "[$_lts_suite] Kyuafile missing at $_lts_dir/Kyuafile. Run --suite $_lts_suite --compile first."
        return 1
    fi

    log_info "Available $_lts_suite tests in $_lts_dir:"

    (
        cd "$_lts_dir" || exit 1

        _lts_list=$(kyua list 2>&1) || {
            log_error "[$_lts_suite] kyua list failed in $_lts_dir"
            printf '%s\n' "$_lts_list"
            exit 1
        }

        echo ""
        printf "  %-38s %-12s %-24s %s\n" "TEST CASE" "CATEGORY" "LABEL" "SEVERITY"
        printf "  %-38s %-12s %-24s %s\n" "---------" "--------" "-----" "--------"
        printf '%s\n' "$_lts_list" | while IFS= read -r tc; do
            BIN=$(printf '%s' "$tc" | cut -d: -f1 | sed 's|.*/||')
            META=$(get_test_meta "$BIN")
            CAT=$(printf '%s' "$META" | cut -d: -f1)
            LABEL=$(printf '%s' "$META" | cut -d: -f2)
            SEV=$(printf '%s' "$META" | cut -d: -f3)
            SCOL=$(severity_color "$SEV")
            printf "  %-38s %-12s %-24s ${SCOL}%s${NC}\n" "$tc" "$CAT" "$LABEL" "$SEV"
        done

        if [ -z "$_lts_list" ]; then
            IMPL=0
        else
            IMPL=$(printf '%s\n' "$_lts_list" | wc -l | tr -d ' ')
        fi
        echo ""
        if [ "$_lts_suite" = "IBS" ]; then
            printf '%s\n' "${YELLOW}═══ PLACEHOLDERS -not yet implemented ═══${NC}"
            printf "  %-38s %-12s %-24s %s\n" "TEST AREA" "CATEGORY" "LABEL" "SEVERITY"
            printf "  %-38s %-12s %-24s %s\n" "---------" "--------" "-----" "--------"
            # Core PMC
            printf "  %-38s %-12s %-24s ${YELLOW}%s${NC}\n" "core_ctr_test  (TODO)" "TC-CORE-CTR"  "Core PMC Counters"   "HIGH"
            printf "  %-38s %-12s %-24s ${YELLOW}%s${NC}\n" "core_filt_test (TODO)" "TC-CORE-FILT" "Kernel/User Filter"  "HIGH"
            printf "  %-38s %-12s %-24s ${YELLOW}%s${NC}\n" "core_smp_test  (TODO)" "TC-CORE-SMP"  "Core PMC SMP"        "HIGH"
            # Uncore PMC
            printf "  %-38s %-12s %-24s ${RED}%s${NC}\n"    "unc_det_test   (TODO)" "TC-UNC-DET"   "Uncore Detection"   "CRITICAL"
            printf "  %-38s %-12s %-24s ${YELLOW}%s${NC}\n" "unc_l3_test    (TODO)" "TC-UNC-L3"    "L3 Cache PMU"        "HIGH"
            printf "  %-38s %-12s %-24s ${YELLOW}%s${NC}\n" "unc_df_test    (TODO)" "TC-UNC-DF"    "Data Fabric PMU"     "HIGH"
            printf "  %-38s %-12s %-24s ${YELLOW}%s${NC}\n" "unc_umc_test   (TODO)" "TC-UNC-UMC"   "Memory Controller"   "HIGH"
            printf "  %-38s %-12s %-24s ${CYAN}%s${NC}\n"   "unc_c2c_test   (TODO)" "TC-UNC-C2C"   "Cache-to-Cache PMU"  "MEDIUM"
            # Misc
            printf "  %-38s %-12s %-24s ${CYAN}%s${NC}\n"   "misc_metrics_test (TODO)" "TC-MISC-METRICS" "Perf Metrics"   "MEDIUM"
            printf "  %-38s %-12s %-24s ${CYAN}%s${NC}\n"   "misc_topdown_test (TODO)" "TC-MISC-TOPDOWN" "Top-Down Analysis" "MEDIUM"
            printf "  %-38s %-12s %-24s ${CYAN}%s${NC}\n"   "misc_proc_test    (TODO)" "TC-MISC-PROC"    "Per-Process PMC"  "MEDIUM"
            printf "  %-38s %-12s %-24s ${CYAN}%s${NC}\n"   "misc_api_test     (TODO)" "TC-MISC-API"     "API Stability"    "MEDIUM"
            echo ""
            echo "  Implemented: $IMPL test cases   Placeholders: 12 test areas"
        else
            echo "  Implemented: $IMPL test cases"
        fi
    )
}

list_tests() {
    _lt_rc=0

    if [ "$SUITE" = "ALL" ]; then
        for _lt_suite in $SUITE_LIST; do
            if ! _lt_dir=$(suite_install_dir "$_lt_suite"); then
                log_error "Unknown suite: $_lt_suite"
                _lt_rc=1
                continue
            fi
            _list_tests_for_suite "$_lt_suite" "$_lt_dir" || _lt_rc=1
        done
        return "$_lt_rc"
    fi

    _lt_suite="$SUITE"
    case "$_lt_suite" in
        DEFAULT) _lt_suite="$_SUITE_PRIMARY" ;;
    esac
    if ! _lt_dir=$(suite_install_dir "$_lt_suite"); then
        log_error "Unknown suite: $_lt_suite"
        return 1
    fi
    _list_tests_for_suite "$_lt_suite" "$_lt_dir"
}

show_report() {
    echo ""
    printf '%s\n' "${CYAN}=================================================================${NC}"
    printf '%s\n' "${WHITE}  LAST RUN REPORT — PANIC/ERROR TRIAGE${NC}"
    printf '%s\n' "${CYAN}=================================================================${NC}"
    printf "  System : %s\n" "$(uname -a)"
    printf "  Now    : %s\n" "$(date)"
    echo ""

    # ── 1. Kernel panic / crash detection ──────────────────────────────────
    printf '%s\n' "${BOLD}[1] KERNEL PANIC / CRASH DETECTION${NC}"
    echo "-----------------------------------------------------------------"

    # Uptime heuristic: short uptime after a test run = likely crash/hard reset
    _up_secs=$(sysctl -n kern.boottime 2>/dev/null | awk '{print $4}' | tr -d ',')
    _now_secs=$(date +%s)
    _uptime_mins=0
    [ -n "$_up_secs" ] && _uptime_mins=$(( (_now_secs - _up_secs) / 60 ))
    if [ "$_uptime_mins" -lt 30 ]; then
        printf '%s\n' "${RED}WARNING: System uptime is only ${_uptime_mins} min — likely a post-crash reboot.${NC}"
    else
        printf "  Uptime: ${_uptime_mins} min\n"
    fi
    echo ""

    # dmesg panic markers
    _panic_lines=$(dmesg 2>/dev/null | grep -iE 'panic:|KDB: enter|Trap [0-9]+|Fatal trap|page fault|double fault|NMI|machine check|watchdog' | tail -20)
    if [ -n "$_panic_lines" ]; then
        printf '%s\n' "${RED}dmesg panic/crash markers:${NC}"
        printf '%s\n' "$_panic_lines"
        echo ""
        printf '%s\n' "${YELLOW}--- dmesg tail (last 40 lines) ---${NC}"
        dmesg 2>/dev/null | tail -40
    else
        printf '%s\n' "${GREEN}No panic/crash markers in dmesg.${NC}"
    fi
    echo ""

    # /var/log/messages — catches watchdog, MCE, hard resets missed by dmesg
    if [ -f /var/log/messages ]; then
        _msg_panic=$(grep -iE 'panic|watchdog|machine check|NMI|fatal|reboot|shutdown' \
            /var/log/messages | tail -20)
        if [ -n "$_msg_panic" ]; then
            printf '%s\n' "${YELLOW}/var/log/messages crash/reboot events:${NC}"
            printf '%s\n' "$_msg_panic"
        else
            printf '%s\n' "${GREEN}No crash/reboot events in /var/log/messages.${NC}"
        fi
        echo ""
    fi

    # vmcore check
    _vmcore=$(ls /var/crash/vmcore* 2>/dev/null | head -5)
    if [ -n "$_vmcore" ]; then
        printf '%s\n' "${RED}Crash dump(s) found in /var/crash/:${NC}"
        ls -lh /var/crash/vmcore* 2>/dev/null
        printf "  Run: kgdb /boot/kernel/kernel /var/crash/vmcore.last\n"
    else
        printf '%s\n' "${YELLOW}No vmcore in /var/crash/ — hard reset or dump not captured.${NC}"
        printf "  Ensure kern.panic_reboot_wait_time >= 30 (currently: %s s)\n" \
            "$(sysctl -n kern.panic_reboot_wait_time 2>/dev/null || echo unknown)"
    fi
    echo ""

    # ── 2. Last test state (surviving across reboots via /var/log) ─────────
    printf '%s\n' "${BOLD}[2] LAST TEST STATE (pre-reboot)${NC}"
    echo "-----------------------------------------------------------------"
    if [ -f "$LAST_TEST_STATE" ]; then
        # If current state has no LAST_COMPLETED (new run just started / was reset),
        # fall back to .prev which holds the pre-crash evidence.
        _last_tc=$(grep '^LAST_COMPLETED=' "$LAST_TEST_STATE" | tail -1 | cut -d= -f2-)
        _sr_state="$LAST_TEST_STATE"
        if [ -z "$_last_tc" ] && [ -f "${LAST_TEST_STATE}.prev" ]; then
            _sr_state="${LAST_TEST_STATE}.prev"
            printf "${YELLOW}  (current state is empty — showing pre-crash .prev file)${NC}\n\n"
        fi
        cat "$_sr_state"
        echo ""
        # Highlight if the last completed test may be the culprit
        _last_tc=$(grep '^LAST_COMPLETED=' "$_sr_state" | tail -1 | cut -d= -f2-)
        _last_st=$(grep '^LAST_STATUS='    "$_sr_state" | tail -1 | cut -d= -f2-)
        _last_at=$(grep '^LAST_SEEN_AT='   "$_sr_state" | tail -1 | cut -d= -f2-)
        if [ -n "$_last_tc" ]; then
            case "$_last_st" in
                failed|broken)
                    printf "${RED}>>> Last recorded test was %s: %s at %s${NC}\n" \
                        "$_last_st" "$_last_tc" "$_last_at"
                    ;;
                passed|skipped|xfail)
                    printf "${YELLOW}>>> Last recorded test was %s: %s at %s${NC}\n" \
                        "$_last_st" "$_last_tc" "$_last_at"
                    printf "    If a panic occurred, the NEXT unrecorded test is the likely culprit.\n"
                    ;;
            esac
        fi
    else
        printf "  No state file found (%s)\n" "$LAST_TEST_STATE"
        printf "  Run tests at least once to populate it.\n"
    fi
    echo ""

    # ── 3. Failures from saved report.txt (survives reboots) ───────────────
    printf '%s\n' "${BOLD}[3] FAILURES FROM SAVED REPORT${NC}"
    echo "-----------------------------------------------------------------"
    # Find the most recent report.txt under HTML_DIR
    _saved_report=""
    if [ -d "$HTML_DIR" ]; then
        _saved_report=$(ls -dt "$HTML_DIR"/results-*/report.txt 2>/dev/null | head -1)
    fi
    # Fallback: check RESULTS_DIR if it already exists from this session
    if [ -z "$_saved_report" ] && [ -f "$RESULTS_DIR/report.txt" ]; then
        _saved_report="$RESULTS_DIR/report.txt"
    fi

    if [ -n "$_saved_report" ] && [ -f "$_saved_report" ]; then
        printf "  Source: %s\n" "$_saved_report"
        printf "  Modified: %s\n\n" "$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$_saved_report" 2>/dev/null || date -r "$_saved_report" 2>/dev/null || echo 'unknown')"

        # Extract verdict line
        _verdict=$(grep 'VERDICT:' "$_saved_report" | tail -1)
        if [ -n "$_verdict" ]; then
            case "$_verdict" in
                *APPROVED*)    printf "${GREEN}%s${NC}\n\n" "$_verdict" ;;
                *CONDITIONAL*) printf "${YELLOW}%s${NC}\n\n" "$_verdict" ;;
                *)             printf "${RED}%s${NC}\n\n" "$_verdict" ;;
            esac
        fi

        # Extract failed/broken test entries
        _failures=$(grep -E '^\s*(FAILED|BROKEN|failed|broken)' "$_saved_report" | head -30)
        if [ -n "$_failures" ]; then
            printf '%s\n' "${RED}--- Failed/Broken tests ---${NC}"
            printf '%s\n' "$_failures"
        else
            # Try kyua-style "result: failed" blocks
            _failures=$(grep -B2 'result: failed\|result: broken' "$_saved_report" | head -40)
            if [ -n "$_failures" ]; then
                printf '%s\n' "${RED}--- Failed/Broken tests ---${NC}"
                printf '%s\n' "$_failures"
            else
                printf '%s\n' "${GREEN}No failures found in saved report.${NC}"
            fi
        fi
    else
        printf "  No saved report.txt found under %s\n" "$HTML_DIR"
        printf "  (Reports are saved after a --run-all or menu option 1 run)\n"
    fi
    echo ""

    # ── 4. Live run log tail (last activity before crash) ──────────────────
    printf '%s\n' "${BOLD}[4] RUN LOG — LAST 60 LINES (pre-reboot activity)${NC}"
    echo "-----------------------------------------------------------------"
    if [ -f "${LAST_RUN_LOG}.prev" ]; then
        _prev_lines=$(wc -l < "${LAST_RUN_LOG}.prev" 2>/dev/null || echo 0)
        printf "${YELLOW}  Pre-crash log: %s.prev  (%d lines total)${NC}\n\n" \
            "$LAST_RUN_LOG" "$_prev_lines"
        tail -60 "${LAST_RUN_LOG}.prev"
        echo ""
        printf "${YELLOW}  --- current log (post-reboot new run) ---${NC}\n"
    fi
    if [ -f "$LAST_RUN_LOG" ]; then
        _log_lines=$(wc -l < "$LAST_RUN_LOG" 2>/dev/null || echo 0)
        printf "  Source: %s  (%d lines total)\n\n" "$LAST_RUN_LOG" "$_log_lines"
        tail -40 "$LAST_RUN_LOG"
    else
        printf "  No run log found (%s)\n" "$LAST_RUN_LOG"
    fi
    echo ""

    # ── 5. Re-generate from kyua db if available ───────────────────────────
    printf '%s\n' "${BOLD}[5] LIVE KYUA REPORT (requires kyua db to be intact)${NC}"
    echo "-----------------------------------------------------------------"
    if [ -d "$TESTS_INSTALL_DIR" ]; then
        mkdir -p "$RESULTS_DIR"
        REPORT_TXT="$RESULTS_DIR/report.txt"
        REPORT_XML="$RESULTS_DIR/report.xml"
        cd "$TESTS_INSTALL_DIR" || true
        if kyua report --verbose --results-filter passed,skipped,xfail,broken,failed \
                2>/dev/null | tee "$REPORT_TXT"; then
            kyua report-junit > "$REPORT_XML" 2>/dev/null || true
            printf "\n"
            log_info "Full report : $REPORT_TXT"
            log_info "JUnit XML   : $REPORT_XML"
        else
            printf '%s\n' "${YELLOW}kyua db unavailable or empty — skipped.${NC}"
            printf "  (This is expected after a kernel panic/reboot)\n"
        fi
    else
        printf "${YELLOW}Tests not installed at %s — skipping live kyua report.${NC}\n" "$TESTS_INSTALL_DIR"
        printf "  Run --compile first if you want live report generation.\n"
    fi

    printf '\n%s\n' "${CYAN}=================================================================${NC}"
}

show_status() {
    echo ""
    echo "================================================================="
    echo "IBS IMPLEMENTATION STATUS"
    echo "================================================================="
    echo "Version: $SCRIPT_VERSION"
    echo "GitHub fork : $REPO_URL ($BRANCH)"
    echo "sos-git     : ssh://git@sos-git.amd.com/freebsd-src.git ($SOS_BRANCH)"
    echo "Architecture: $ARCH"

    # CPU vendor and IBS feature-bit context
    VENDOR=$(get_cpu_vendor)
    printf "CPU Vendor  : %s\n" "$VENDOR"
    case "$VENDOR" in
        AuthenticAMD|AMD)
            printf "IBS Support : ${GREEN}Hardware (AMD)${NC}\n"
            printf "  CPUBIT IBS_SUPPORT   [CPUID 8000_0001h ECX bit 10] = ${GREEN}on${NC}\n"
            printf "  CPUBIT IbsFetchEnable [MSR C001_1030h bit 18]       = ${GREEN}on${NC} (HW)\n"
            printf "  CPUBIT IbsOpEnable    [MSR C001_1033h bit 19]       = ${GREEN}on${NC} (HW)\n"
            ;;
        *)
            printf "IBS Support : ${YELLOW}None (non-AMD CPU -all IBS CPUITs off)${NC}\n"
            printf "  CPUBIT IBS_SUPPORT    [CPUID 8000_0001h ECX bit 10] = ${RED}off${NC}\n"
            printf "  CPUBIT IbsFetchEnable [MSR C001_1030h bit 18]       = ${RED}off${NC}\n"
            printf "  CPUBIT IbsOpEnable    [MSR C001_1033h bit 19]       = ${RED}off${NC}\n"
            ;;
    esac
    echo ""

    # Check source repository status
    if [ -d "$SRC_DIR/.git" ]; then
        cd "$SRC_DIR" || true
        LAST_COMMIT=$(git log -1 --format="%h - %s (%cd)" --date=format:'%Y-%m-%d %H:%M')
        echo "GitHub fork : synced -$LAST_COMMIT"
    else
        echo "GitHub fork : not synced (run --download)"
    fi

    # Check sos-git status
    if [ -d "$SOS_DIR/.git" ]; then
        cd "$SOS_DIR" || true
        SOS_COMMIT=$(git log -1 --format="%h - %s (%cd)" --date=format:'%Y-%m-%d %H:%M')
        echo "sos-git     : $SOS_COMMIT"
    else
        echo "sos-git     : not found at $SOS_DIR"
    fi

    # Check test build status
    if [ -d "$TESTS_INSTALL_DIR" ]; then
        TEST_COUNT=$(find "$TESTS_INSTALL_DIR" -name "*test" -type f | wc -l)
        echo "Tests Built : installed ($TEST_COUNT test files in $TESTS_INSTALL_DIR)"
    else
        echo "Tests Built : not built (run --compile)"
    fi

    # Hardware detection
    if [ -c /dev/cpuctl0 ]; then
        echo "Hardware    : cpuctl available (/dev/cpuctl0)"
        if command -v rdmsr >/dev/null 2>&1; then
            if rdmsr 0xC0011030 >/dev/null 2>&1; then
                echo "IBS MSR     : accessible"
            else
                echo "IBS MSR     : access failed"
            fi
        else
            echo "IBS MSR     : rdmsr not available"
        fi
    else
        echo "Hardware    : cpuctl not available (run --loadmodule)"
    fi

    echo ""
    echo "Next Steps:"
    if [ ! -c /dev/cpuctl0 ]; then
        echo "  0. Run '$0 --loadmodule' to load cpuctl for hardware access"
    fi
    if [ ! -d "$SRC_DIR/.git" ]; then
        echo "  1. Run '$0 --download' to sync source"
    fi
    echo "  2. Run '$0 --compile' to build tests"
    echo "  3. Run '$0 --run-all' to execute test suite"
    echo "  4. Run '$0 --commit' to push to sos-git"
    echo ""
    echo "================================================================="
}

clean_artifacts() {
    log_info "Cleaning build artifacts..."

    if [ -d "$TESTS_DIR" ]; then
        cd "$TESTS_DIR" || exit 1
        if [ $DRY_RUN -eq 0 ]; then
            make clean
            log_success "Build artifacts cleaned from $TESTS_DIR"
        else
            log_info "Would clean build artifacts (dry run)"
        fi
    fi

    if [ -d "$HTML_DIR" ] && [ $DRY_RUN -eq 0 ]; then
        log_info "HTML dir preserved: $HTML_DIR (remove results manually if needed)"
    fi
}

load_module() {
    if [ $DRY_RUN -eq 1 ]; then
        log_info "Would load cpuctl and hwpmc modules (dry run)"
        return 0
    fi

    log_info "Loading cpuctl kernel module..."
    if kldstat -q -n cpuctl 2>/dev/null; then
        log_success "cpuctl module already loaded"
    else
        confirm_cmd "Load cpuctl kernel module (required for MSR/CPUID access)" \
            "kldload cpuctl" || return 1
        kldload cpuctl || {
            log_error "Failed to load cpuctl module"
            log_info "Ensure cpuctl is available in the kernel or as a module"
            exit 1
        }
        log_success "cpuctl module loaded successfully"
    fi

    log_info "Loading hwpmc kernel module..."
    if kldstat -q -n hwpmc 2>/dev/null; then
        log_success "hwpmc module already loaded"
    else
        confirm_cmd "Load hwpmc kernel module (required for PMC API tests)" \
            "kldload hwpmc" || return 1
        kldload hwpmc 2>/dev/null || {
            log_warning "Failed to load hwpmc -TC-HWPMC tests will skip"
            return 0
        }
        log_success "hwpmc module loaded successfully"
    fi
}

# Interactive menu
show_menu() {
    while true; do
        clear
        printf '%s\n' "${CYAN}=================================================================${NC}"
        printf '%s\n' "${WHITE}  IBS Test Suite Manager v${SCRIPT_VERSION}${NC}"
        printf '%s\n' "${CYAN}=================================================================${NC}"

        # System info
        printf "  System : %s\n" "$(uname -rs)"

        # CPU vendor
        _vendor=$(get_cpu_vendor)
        case "$_vendor" in
            AuthenticAMD|AMD)
                printf "  CPU    : ${GREEN}%s -hardware IBS active${NC}\n" "$_vendor" ;;
            *)
                printf "  CPU    : ${YELLOW}%s -no hardware IBS${NC}\n" "$_vendor" ;;
        esac

        # cpuctl module
        if kldstat -q -n cpuctl 2>/dev/null || [ -c /dev/cpuctl0 ]; then
            printf "  cpuctl : ${GREEN}loaded${NC}\n"
        else
            printf "  cpuctl : ${RED}NOT loaded${NC}\n"
        fi

        # Tests installed
        if [ -d "$TESTS_INSTALL_DIR" ]; then
            _cnt=$(find "$TESTS_INSTALL_DIR" -name "*test" -type f 2>/dev/null | wc -l | tr -d ' ')
            printf "  Tests  : ${GREEN}installed (%s binaries in %s)${NC}\n" "$_cnt" "$TESTS_INSTALL_DIR"
        else
            printf "  Tests  : ${RED}not installed -choose option 3 to compile${NC}\n"
        fi

        # Source location
        if [ -d "$SRC_DIR/.git" ]; then
            _commit=$(git -C "$SRC_DIR" log -1 --format="%h %cd" --date=format:'%Y-%m-%d' 2>/dev/null)
            printf "  Source : dev/freebsd synced (%s)\n" "$_commit"
        else
            printf "  Source : local tests/ (repo checkout)\n"
        fi

        printf '%s\n' "${CYAN}=================================================================${NC}"
        printf "  Suites : ${BOLD}%s${NC}  (change with --suite IBS|UMCDF|PMC|TSC|L3|STRESS|ALL)\n" "$SUITE_LIST"
        printf '\n'
        printf '  %s1)%s Run all tests (suite: %s)\n'      "$BOLD" "$NC" "$SUITE"
        printf '  %s2)%s Run specific test\n'              "$BOLD" "$NC"
        printf '  %s3)%s Run by category\n'                "$BOLD" "$NC"
        printf '  %s4)%s Compile & install tests\n'        "$BOLD" "$NC"
        printf '  %s5)%s Download source from GitHub\n'    "$BOLD" "$NC"
        printf '  %s6)%s Load kernel module (cpuctl)\n'    "$BOLD" "$NC"
        printf '  %s7)%s Show status\n'                    "$BOLD" "$NC"
        printf '  %s8)%s List tests\n'                     "$BOLD" "$NC"
        printf '  %sr)%s View last report\n'               "$BOLD" "$NC"
        printf '  %s9)%s Commit to sos-git\n'              "$BOLD" "$NC"
        printf '  %sf)%s Fetch from GitHub (origin main)\n' "$BOLD" "$NC"
        printf '  %sp)%s Push to GitHub (origin main)\n'   "$BOLD" "$NC"
        printf '  %sa)%s AUTO: build kernel + reboot + test + email\n' "$BOLD" "$NC"
        printf '  %s0)%s Exit\n'                           "$BOLD" "$NC"
        printf '\n'
        printf '%sChoice: %s' "$BOLD" "$NC"
        read -r MENU_CHOICE
        printf '\n'

        case "$MENU_CHOICE" in
            1)
                check_root_privileges
                _idir=$(suite_install_dir "$_SUITE_PRIMARY")
                TESTS_INSTALL_DIR="$_idir"
                run_all_tests
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            2)
                printf '%sTest name: %s' "$BOLD" "$NC"
                read -r _test
                if [ -n "$_test" ]; then
                    check_root_privileges
                    _idir=$(suite_install_dir "$_SUITE_PRIMARY")
                    TESTS_INSTALL_DIR="$_idir"
                    run_specific_test "$_test"
                else
                    log_error "No test name given"
                fi
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            3)
                printf '%sCategories (space-separated, e.g. TC-DET TC-MSR): %s' "$BOLD" "$NC"
                read -r _cats
                if [ -n "$_cats" ]; then
                    for _cat in $_cats; do
                        if ! valid_category_token "$_cat"; then
                            log_error "Invalid category: $_cat"
                            continue 2
                        fi
                    done
                    check_root_privileges
                    CATEGORIES="$_cats"
                    _idir=$(suite_install_dir "$_SUITE_PRIMARY")
                    TESTS_INSTALL_DIR="$_idir"
                    run_all_tests
                    CATEGORIES=""
                else
                    log_error "No categories given"
                fi
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            4)
                check_boot_environment
                check_root_privileges
                TESTS_DIR=$(suite_src_dir "$_SUITE_PRIMARY")
                TESTS_INSTALL_DIR=$(suite_install_dir "$_SUITE_PRIMARY")
                compile_tests
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            5)
                check_boot_environment
                check_root_privileges
                sync_repository
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            6)
                check_root_privileges
                load_module
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            7)
                show_status
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            8)
                _idir=$(suite_install_dir "$_SUITE_PRIMARY")
                TESTS_INSTALL_DIR="$_idir"
                list_tests
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            r|R)
                show_report
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            9)
                commit_to_sos
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            f|F)
                fetch_from_remote
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            p|P)
                push_to_remote
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            a|A)
                printf '%sEmail for report [%s]: %s' "$BOLD" "$REPORT_EMAIL" "$NC"
                read -r _mail
                [ -n "$_mail" ] && REPORT_EMAIL="$_mail"
                printf '%sKernel config [%s]: %s' "$BOLD" "$AUTO_KERNCONF" "$NC"
                read -r _kc
                [ -n "$_kc" ] && AUTO_KERNCONF="$_kc"
                auto_mode "$REPORT_EMAIL"
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
            0|q|Q)
                printf '%s\n' "${GREEN}Bye.${NC}"
                exit 0
                ;;
            *)
                log_error "Invalid option: '$MENU_CHOICE'"
                printf '\nPress Enter to return to menu...'
                read -r _dummy
                ;;
        esac
    done
}

# Main argument parsing
COMMAND=""
while [ $# -gt 0 ]; do
    case $1 in
        --download)
            COMMAND="download"
            ;;
        --compile)
            COMMAND="compile"
            ;;
        --run-all)
            COMMAND="run-all"
            ;;
        --run)
            shift
            if [ $# -eq 0 ]; then
                log_error "--run requires a test name"
                exit 1
            fi
            SPECIFIC_TEST="$1"
            if [ -z "$SPECIFIC_TEST" ]; then
                log_error "--run requires a non-empty test name"
                exit 1
            fi
            case "$SPECIFIC_TEST" in
                -*) log_error "--run: test name cannot start with '-': $SPECIFIC_TEST"; exit 1 ;;
            esac
            COMMAND="run-specific"
            ;;
        --list)
            COMMAND="list"
            ;;
        --report)
            COMMAND="report"
            ;;
        --status)
            COMMAND="status"
            ;;
        --clean)
            COMMAND="clean"
            ;;
        --loadmodule)
            COMMAND="loadmodule"
            ;;
        --fetch)
            COMMAND="fetch"
            ;;
        --push)
            COMMAND="push"
            ;;
        --commit)
            COMMAND="commit"
            ;;
        --auto)
            COMMAND="auto"
            AUTO_MODE=1
            ;;
        --suite)
            shift
            if [ $# -eq 0 ]; then
                log_error "--suite requires IBS|UMCDF|PMC|TSC|L3|STRESS|ALL"
                exit 1
            fi
            case "$1" in
                IBS|UMCDF|PMC|TSC|L3|STRESS|ALL|DEFAULT) SUITE="$1" ;;
                *) log_error "--suite: unknown suite '$1' (use IBS|UMCDF|PMC|TSC|L3|STRESS|ALL)"; exit 1 ;;
            esac
            ;;
        --category)
            shift
            if [ $# -eq 0 ]; then
                log_error "--category requires a TC-* code"
                exit 1
            fi
            if ! valid_category_token "$1"; then
                log_error "--category: invalid category '$1' (use TC-* with letters, digits, '_' or '-')"
                exit 1
            fi
            CATEGORIES="${CATEGORIES} $1"
            ;;
        --email)
            shift
            if [ $# -eq 0 ]; then
                log_error "--email requires an address"
                exit 1
            fi
            REPORT_EMAIL="$1"
            EMAIL_REQUESTED=1
            ;;
        --kernconf)
            shift
            if [ $# -eq 0 ]; then
                log_error "--kernconf requires a config name"
                exit 1
            fi
            AUTO_KERNCONF="$1"
            ;;
        -v|--verbose)
            VERBOSE=1
            ;;
        -f|--force)
            FORCE=1
            ;;
        --force-rebuild)
            FORCE_REBUILD=1
            ;;
        -n|--dry-run)
            DRY_RUN=1
            ;;
        --parallelism)
            shift
            if [ $# -eq 0 ]; then
                log_error "--parallelism requires a value"
                exit 1
            fi
            PARALLELISM="$1"
            case "$PARALLELISM" in
                ''|*[!0-9]*) log_error "--parallelism requires a positive integer, got: '$PARALLELISM'"; exit 1 ;;
            esac
            ;;
        --results-dir)
            shift
            if [ $# -eq 0 ]; then
                log_error "--results-dir requires a path"
                exit 1
            fi
            RESULTS_DIR="$1"
            ;;
        --stress)
            WITH_STRESS=1
            ;;
        --safe-ibs-rate)
            shift
            if [ $# -eq 0 ]; then
                log_error "--safe-ibs-rate requires a period value (e.g. 0x1000)"
                exit 1
            fi
            SAFE_IBS_RATE="$1"
            ;;
        --last-test)
            COMMAND="last-test"
            ;;
        --reindex)
            COMMAND="reindex"
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
    shift
done

# Expand SUITE to the list of suites that will be run.
if ! SUITE_LIST=$(expand_suite_list "$SUITE"); then
    log_error "Unknown suite selector: $SUITE"
    exit 1
fi
# For commands that operate on a single suite (--compile, --list, etc.),
# use the first suite in the list.
_SUITE_PRIMARY=$(printf '%s' "$SUITE_LIST" | awk '{print $1}')
if ! TESTS_DIR=$(suite_src_dir "$_SUITE_PRIMARY"); then
    log_error "Unknown primary suite: $_SUITE_PRIMARY"
    exit 1
fi
if ! TESTS_INSTALL_DIR=$(suite_install_dir "$_SUITE_PRIMARY"); then
    log_error "Unknown primary suite: $_SUITE_PRIMARY"
    exit 1
fi

if [ "$COMMAND" = "run-specific" ] && [ "$SUITE" = "ALL" ]; then
    log_error "--run requires a concrete suite; use --suite IBS|UMCDF|PMC|TSC|L3|STRESS."
    exit 1
fi

# Execute command
case $COMMAND in
    download)
        check_boot_environment
        check_root_privileges
        sync_repository
        ;;
    compile)
        check_boot_environment
        check_root_privileges
        compile_tests
        ;;
    run-all)
        check_root_privileges
        run_all_tests
        ;;
    run-specific)
        check_root_privileges
        run_specific_test "$SPECIFIC_TEST"
        ;;
    list)
        list_tests
        ;;
    report)
        show_report
        ;;
    status)
        show_status
        ;;
    clean)
        clean_artifacts
        ;;
    loadmodule)
        load_module
        ;;
    fetch)
        fetch_from_remote
        ;;
    push)
        push_to_remote
        ;;
    commit)
        commit_to_sos
        ;;
    auto)
        auto_mode "$REPORT_EMAIL"
        ;;
    last-test)
        show_last_test
        ;;
    reindex)
        generate_html_index
        ;;
    *)
        if [ -z "$COMMAND" ]; then
            show_menu
        fi
        ;;
esac

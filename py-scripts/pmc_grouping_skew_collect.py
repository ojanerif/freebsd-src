#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
# Special thanks: ojanerif@amd.com for identifying deterministic
# sequential-grouping behavior in the PMC skew measurements.
#
# Purpose:
#   Canonical AMD duplicate-core-PMC skew collector for FreeBSD hwpmc(4).
#
#   The collector measures two process-scope pmcstat(8) rows programmed for the
#   same AMD Zen core event, FreeBSD event name `ls_not_halted_cyc` (silicon:
#   PMCx076, core cycles not in halt).  It first calibrates the deterministic
#   signed offset introduced by sequential pmcstat/libpmc/hwpmc row handling,
#   then subtracts that offset from a CPU-bound measurement workload.
#
#   This is intentionally the only pmc_grouping_skew_collect.py implementation:
#   historical v1/v2/v3/v4 collectors were investigation scaffolding.  This file
#   is the precise validation path.

"""Collect calibrated FreeBSD AMD PMC grouping skew data.

Silicon: AMD Zen PMCx076 counts core cycles not in halt.
FreeBSD: pmcstat exposes that event as ``ls_not_halted_cyc``.
Tooling: ``pmcstat -C -q -p EVENT -p EVENT -- workload`` starts two process
counting rows sequentially, so raw duplicate counts include a fixed signed
measurement-path offset.  The corrected metric below removes that offset:

    corrected_delta = abs(measured_signed_delta - baseline_signed_offset)
    corrected_permille = corrected_delta / max(last_a, last_b) * 1000

No shell strings are executed; every command is argv-based.  Hardware-sensitive
probes run serially by design.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import platform
import re
import signal
import shutil
import shlex
import statistics
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from pmu_common.amd_zen import parse_hwpmc_cpuid
from pmu_common.command import (
    CommandRunner,
    SudoConfig,
    bounded_timeout,
    check_tool as common_check_tool,
    is_root,
    remaining_seconds,
    simple_command_value,
)
from pmu_common.fs import (
    atomic_write_file,
    atomic_write_text,
    fsync_directory,
    read_text_file,
    sha256_file,
    unlink_suppress,
    unique_tmp_path,
    utc_now,
)
from pmu_common.metrics import percentile
from pmu_common.terminal import Terminal


SCHEMA_VERSION = 4
PRODUCER = "pmc_grouping_skew_collect.py"
EVENT = "ls_not_halted_cyc"
SILICON_EVENT = "PMCx076"

DEFAULT_CALIBRATION_ITERATIONS = 50
DEFAULT_MEASUREMENT_ITERATIONS = 50
DEFAULT_DD_COUNT = 500_000
DEFAULT_MIN_VALID_SAMPLES = 5
DEFAULT_MIN_MEASUREMENT_CYCLES = 1_000_000
DEFAULT_MINUTES = 30.0
DEFAULT_CORRECTED_TOL_PERMILLE = 1.0
DEFAULT_VERDICT_PERCENTILE = 0.95
DEFAULT_OFFSET_STABILITY_WARN_PCT = 30.0
DEFAULT_PREFLIGHT_TIMEOUT = 30
DEFAULT_COMMAND_TIMEOUT_OVERHEAD = 30
DEFAULT_COMMAND_GRACE_SECONDS = 10

STOP_REQUESTED = False
SHUTDOWN_REASON: Optional[str] = None
TERMINAL = Terminal("pmc-skew")
COMMAND_RUNNER = CommandRunner(TERMINAL)
ACTIVE_DASHBOARD: Optional["LiveDashboard"] = None


def log_info(message: str) -> None:
    TERMINAL.info(message)


def log_warn(message: str) -> None:
    TERMINAL.warn(message)


def log_error(message: str) -> None:
    TERMINAL.error(message)


def log_verbose(message: str) -> None:
    TERMINAL.verbose(message)


def die(message: str, exit_code: int = 1) -> None:
    log_error(message)
    raise SystemExit(exit_code)


def parse_positive_int(value: str, name: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must be an integer") from exc
    if number <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")
    return number


def parse_nonnegative_float(value: str, name: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must be a number") from exc
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError(f"{name} must be non-negative")
    return number


def parse_positive_float(value: str, name: str) -> float:
    number = parse_nonnegative_float(value, name)
    if number <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")
    return number


def parse_fraction(value: str, name: str) -> float:
    number = parse_positive_float(value, name)
    if number > 1.0:
        raise argparse.ArgumentTypeError(f"{name} must be in the range (0, 1]")
    return number


def configure_terminal(args: argparse.Namespace) -> None:
    TERMINAL.configure(
        color=args.color,
        live_graph=args.live_graph,
        verbose=args.verbose,
        quiet=args.quiet,
    )


def sysctl_value(name: str) -> str:
    return simple_command_value(["sysctl", "-n", name])


def check_tool(name: str) -> Optional[str]:
    try:
        return common_check_tool(name, required=True)
    except FileNotFoundError:
        die(f"required tool not found in PATH: {name}")


def resolve_tool_paths(args: argparse.Namespace) -> None:
    """Resolve command paths once so privileged execution is PATH-stable."""
    if args.dry_run:
        paths = {
            "pmcstat": "pmcstat",
            "kldstat": "kldstat",
            "kldload": "kldload",
            "true": "true",
            "dd": "dd",
        }
    else:
        required = ["pmcstat", "true", "dd"]
        if platform.system() == "FreeBSD" and not args.no_hwpmc_load:
            required.extend(["kldstat", "kldload"])
        paths = {name: check_tool(name) or name for name in required}
        paths.setdefault("kldstat", "kldstat")
        paths.setdefault("kldload", "kldload")

    args.tool_paths = paths
    args.pmcstat_cmd = paths["pmcstat"]
    args.kldstat_cmd = paths["kldstat"]
    args.kldload_cmd = paths["kldload"]
    args.true_cmd = paths["true"]
    args.dd_cmd = paths["dd"]


def command_with_sudo(argv: Sequence[str], args: argparse.Namespace) -> List[str]:
    return SudoConfig(
        use_sudo=args.use_sudo,
        sudo_cmd=args.sudo_cmd,
        non_interactive=args.sudo_non_interactive,
    ).apply(argv)


def run_command(
    argv: Sequence[str],
    args: argparse.Namespace,
    *,
    timeout_seconds: Optional[float] = None,
    progress_label: Optional[str] = None,
    dry_run: Optional[bool] = None,
) -> Dict[str, Any]:
    return COMMAND_RUNNER.run(
        command_with_sudo(argv, args),
        dry_run=args.dry_run if dry_run is None else dry_run,
        timeout_seconds=timeout_seconds,
        grace_seconds=args.command_grace_seconds,
        progress_label=progress_label,
    )


def enough_time(args: argparse.Namespace, needed_seconds: float) -> bool:
    if args.dry_run:
        return True
    remaining = remaining_seconds(args.deadline)
    return remaining is None or remaining > max(1.0, needed_seconds)


def ensure_hwpmc_loaded(args: argparse.Namespace) -> Dict[str, Any]:
    status: Dict[str, Any] = {
        "checked": False,
        "was_loaded": False,
        "load_attempted": False,
        "loaded_by_collector": False,
        "error": None,
    }

    if args.dry_run or args.no_hwpmc_load:
        return status
    if platform.system() != "FreeBSD":
        status["error"] = "not FreeBSD"
        return status

    status["checked"] = True
    loaded = COMMAND_RUNNER.run(
        [args.kldstat_cmd, "-n", "hwpmc"],
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if loaded.get("returncode") == 0:
        status["was_loaded"] = True
        return status

    status["load_attempted"] = True
    load = run_command(
        [args.kldload_cmd, "hwpmc"],
        args,
        timeout_seconds=args.preflight_timeout,
    )
    if load.get("returncode") == 0:
        status["loaded_by_collector"] = True
    else:
        status["error"] = (load.get("stderr") or "kldload hwpmc failed").strip()
    return status


def collect_platform_info(args: argparse.Namespace) -> Dict[str, Any]:
    cpuid_raw = "" if args.dry_run else sysctl_value("kern.hwpmc.cpuid")
    parsed_cpuid = parse_hwpmc_cpuid(cpuid_raw) if cpuid_raw else parse_hwpmc_cpuid("")
    return {
        "collected_at": utc_now(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "uname": " ".join(platform.uname()),
        "hostname": platform.node(),
        "hw_model": "" if args.dry_run else sysctl_value("hw.model"),
        "hw_ncpu": "" if args.dry_run else sysctl_value("hw.ncpu"),
        "hw_cpu_vendor": "" if args.dry_run else sysctl_value("hw.cpu_vendor"),
        "kern_hwpmc_cpuid": cpuid_raw,
        "parsed_cpuid": parsed_cpuid,
        "tool_paths": getattr(args, "tool_paths", {}),
        "pmcstat_path": getattr(args, "pmcstat_cmd", None) or shutil.which("pmcstat"),
    }


def validate_platform(info: Dict[str, Any], args: argparse.Namespace) -> None:
    if args.dry_run:
        return

    if info.get("system") != "FreeBSD" and not args.allow_non_freebsd:
        die("this collector must run on FreeBSD")

    cpuid = info.get("parsed_cpuid", {})
    if cpuid.get("vendor") != "AuthenticAMD" and not args.allow_non_amd:
        die("AMD core PMC collection requires AuthenticAMD CPU and kern.hwpmc.cpuid")

    generation = str(cpuid.get("generation", "unknown"))
    if (
        (generation.startswith("unknown") or generation.startswith("future"))
        and not args.allow_unknown_generation
        and not args.allow_non_amd
    ):
        die(
            "unknown AMD family/model; verify the processor PPR and pass "
            "--allow-unknown-generation only after confirming event semantics"
        )


def collect_event_list(args: argparse.Namespace) -> List[str]:
    if args.dry_run:
        return []
    result = run_command(
        [args.pmcstat_cmd, "-L"],
        args,
        timeout_seconds=args.preflight_timeout,
    )
    if result.get("returncode") != 0:
        log_warn(f"pmcstat -L failed: {(result.get('stderr') or '').strip()}")
        return []
    return [line.split()[0] for line in result.get("stdout", "").splitlines() if line.split()]


def check_event_availability(events: Iterable[str], required: Iterable[str]) -> Dict[str, bool]:
    available = set(events)
    return {event: event in available for event in required}


def parse_pmcstat_two_counter(text: str) -> Optional[Dict[str, Any]]:
    last_a: Optional[int] = None
    last_b: Optional[int] = None
    rows = 0

    for line in text.splitlines():
        if line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[0].isdigit() and fields[1].isdigit():
            rows += 1
            last_a, last_b = int(fields[0]), int(fields[1])

    if last_a is None or last_b is None:
        return None

    max_count = max(last_a, last_b)
    delta_abs = abs(last_a - last_b)
    signed_delta = last_b - last_a
    permille = (delta_abs * 1000.0 / max_count) if max_count > 0 else 0.0

    return {
        "row_count": rows,
        "last_a": last_a,
        "last_b": last_b,
        "max_count": max_count,
        "delta_abs": delta_abs,
        "signed_delta": signed_delta,
        "permille": permille,
        "b_gt_a": last_b > last_a,
        "a_gt_b": last_a > last_b,
        "equal": last_a == last_b,
    }


def run_probe(
    args: argparse.Namespace,
    *,
    phase: str,
    iteration: int,
    workload: Sequence[str],
    raw_dir: Path,
    run_id: str,
    timeout_hint: float,
    progress_label: Optional[str] = None,
) -> Dict[str, Any]:
    output_file = raw_dir / f"{run_id}-{phase}-{iteration:04d}.out"
    tmp_out = output_file if args.dry_run else unique_tmp_path(output_file, "out")
    command = [
        args.pmcstat_cmd, "-C", "-q",
        "-p", EVENT,
        "-p", EVENT,
        "-o", str(tmp_out),
        "--",
        *workload,
    ]
    timeout_seconds = bounded_timeout(
        args.deadline,
        timeout_hint + args.command_timeout_overhead,
    )

    result = run_command(
        command,
        args,
        timeout_seconds=timeout_seconds,
        progress_label=progress_label,
    )
    returncode = result.get("returncode")

    if not args.dry_run and returncode == 0 and tmp_out.exists():
        tmp_out.replace(output_file)
        fsync_directory(output_file.parent)
    elif not args.dry_run:
        unlink_suppress(tmp_out)

    output_text = "" if args.dry_run else read_text_file(output_file)
    parsed = parse_pmcstat_two_counter(output_text) if output_text else None
    valid = bool(parsed and returncode == 0)

    record: Dict[str, Any] = {
        "run_id": run_id,
        "phase": phase,
        "iteration": iteration,
        "workload": list(workload),
        "command": result.get("command"),
        "started_at": result.get("started_at"),
        "ended_at": result.get("ended_at"),
        "elapsed_seconds": result.get("elapsed_seconds"),
        "returncode": returncode,
        "command_outcome": result.get("outcome"),
        "timed_out": result.get("timed_out"),
        "terminated_by_signal": result.get("terminated_by_signal"),
        "valid": valid,
        "row_count": parsed.get("row_count") if parsed else None,
        "last_a": parsed.get("last_a") if parsed else None,
        "last_b": parsed.get("last_b") if parsed else None,
        "max_count": parsed.get("max_count") if parsed else None,
        "delta_abs": parsed.get("delta_abs") if parsed else None,
        "signed_delta": parsed.get("signed_delta") if parsed else None,
        "permille": parsed.get("permille") if parsed else None,
        "b_gt_a": parsed.get("b_gt_a") if parsed else None,
        "a_gt_b": parsed.get("a_gt_b") if parsed else None,
        "equal": parsed.get("equal") if parsed else None,
        "baseline_signed_offset": None,
        "corrected_signed_delta": None,
        "corrected_delta": None,
        "corrected_permille": None,
        "output_file": str(output_file) if not args.dry_run else None,
        "raw_sha256": sha256_file(output_file) if (not args.dry_run and output_file.exists()) else None,
        "stderr_tail": (result.get("stderr") or "")[-400:],
    }
    return record


def run_phase(
    args: argparse.Namespace,
    *,
    phase: str,
    iterations: int,
    workload: Sequence[str],
    raw_dir: Path,
    run_id: str,
    timeout_hint: float,
    dashboard: Optional[LiveDashboard] = None,
    baseline_signed_offset: Optional[float] = None,
) -> List[Dict[str, Any]]:
    records: List[Dict[str, Any]] = []
    if dashboard is not None:
        dashboard.suspend_for_log()
    log_info(f"{phase}: workload={shlex.join(workload)} iterations={iterations}")
    for iteration in range(1, iterations + 1):
        if STOP_REQUESTED:
            break
        if not enough_time(args, timeout_hint + args.command_timeout_overhead):
            if dashboard is not None:
                dashboard.suspend_for_log()
            log_warn(f"{phase}: stopping before iteration {iteration}; deadline too close")
            break
        if dashboard is not None:
            dashboard.start_probe(
                phase=phase,
                records=records,
                requested=iterations,
                iteration=iteration,
                baseline_signed_offset=baseline_signed_offset,
            )
            if args.dry_run:
                dashboard.suspend_for_log()
        record = run_probe(
            args,
            phase=phase,
            iteration=iteration,
            workload=workload,
            raw_dir=raw_dir,
            run_id=run_id,
            timeout_hint=timeout_hint,
            progress_label=None if args.quiet or (dashboard is not None and dashboard.enabled) else f"{phase} {iteration}/{iterations}",
        )
        records.append(record)
        if baseline_signed_offset is not None:
            apply_record_correction(record, baseline_signed_offset)
        if dashboard is not None:
            dashboard.finish_probe(
                records=records,
                baseline_signed_offset=baseline_signed_offset,
            )
        if dashboard is not None and dashboard.enabled:
            continue
        if record["valid"]:
            direction = "b>a" if record["b_gt_a"] else ("a>b" if record["a_gt_b"] else "eq")
            log_verbose(
                f"{phase} iter={iteration}: delta={record['delta_abs']} "
                f"signed={record['signed_delta']} raw={record['permille']:.4f}‰ {direction}"
            )
        else:
            log_verbose(f"{phase} iter={iteration}: invalid outcome={record['command_outcome']}")
    return records


def valid_records(records: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    return [record for record in records if record.get("valid")]


def numeric_stats(values: Sequence[float]) -> Dict[str, Any]:
    clean = [float(value) for value in values if value is not None and math.isfinite(float(value))]
    if not clean:
        return {"n": 0}
    return {
        "n": len(clean),
        "min": min(clean),
        "mean": statistics.fmean(clean),
        "median": statistics.median(clean),
        "p90": percentile(clean, 0.90),
        "p95": percentile(clean, 0.95),
        "max": max(clean),
        "stdev": statistics.pstdev(clean) if len(clean) > 1 else 0.0,
    }


def phase_stats(records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    valid = valid_records(records)
    total = len(records)
    n = len(valid)
    b_gt = sum(1 for record in valid if record.get("b_gt_a"))
    a_gt = sum(1 for record in valid if record.get("a_gt_b"))
    equal = sum(1 for record in valid if record.get("equal"))

    def field(name: str) -> Dict[str, Any]:
        return numeric_stats([record[name] for record in valid if record.get(name) is not None])

    return {
        "records": total,
        "valid": n,
        "invalid": total - n,
        "b_gt_a": b_gt,
        "a_gt_b": a_gt,
        "equal": equal,
        "pct_b_gt_a": (b_gt / n * 100.0) if n else None,
        "last_a": field("last_a"),
        "last_b": field("last_b"),
        "max_count": field("max_count"),
        "delta_abs": field("delta_abs"),
        "signed_delta": field("signed_delta"),
        "permille": field("permille"),
    }


def phase_complete(records: Sequence[Dict[str, Any]], requested: int) -> bool:
    return len(records) >= requested


def calibration_offsets(cal_records: Sequence[Dict[str, Any]]) -> Tuple[float, float]:
    calibration = valid_records(cal_records)
    abs_offsets = [record["delta_abs"] for record in calibration if record.get("delta_abs") is not None]
    signed_offsets = [record["signed_delta"] for record in calibration if record.get("signed_delta") is not None]
    baseline_abs_offset = statistics.median(abs_offsets) if abs_offsets else 0.0
    baseline_signed_offset = statistics.median(signed_offsets) if signed_offsets else 0.0
    return float(baseline_abs_offset), float(baseline_signed_offset)


def apply_record_correction(record: Dict[str, Any], baseline_signed_offset: float) -> None:
    if not record.get("valid"):
        return
    signed_delta = record.get("signed_delta")
    max_count = record.get("max_count")
    if signed_delta is None or not max_count:
        return
    corrected_signed = float(signed_delta) - float(baseline_signed_offset)
    corrected_delta = abs(corrected_signed)
    record["baseline_signed_offset"] = baseline_signed_offset
    record["corrected_signed_delta"] = corrected_signed
    record["corrected_delta"] = corrected_delta
    record["corrected_permille"] = corrected_delta * 1000.0 / float(max_count)


def apply_correction(
    cal_records: Sequence[Dict[str, Any]],
    meas_records: List[Dict[str, Any]],
) -> Tuple[float, float]:
    baseline_abs_offset, baseline_signed_offset = calibration_offsets(cal_records)
    for record in meas_records:
        apply_record_correction(record, baseline_signed_offset)
    return baseline_abs_offset, baseline_signed_offset


def corrected_stats(meas_records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    valid = [record for record in valid_records(meas_records) if record.get("corrected_permille") is not None]
    return {
        "corrected_signed_delta": numeric_stats([
            record["corrected_signed_delta"] for record in valid
            if record.get("corrected_signed_delta") is not None
        ]),
        "corrected_delta": numeric_stats([
            record["corrected_delta"] for record in valid
            if record.get("corrected_delta") is not None
        ]),
        "corrected_permille": numeric_stats([
            record["corrected_permille"] for record in valid
            if record.get("corrected_permille") is not None
        ]),
    }


def verdict_from_stats(
    cal_records: Sequence[Dict[str, Any]],
    meas_records: Sequence[Dict[str, Any]],
    baseline_signed_offset: float,
    args: argparse.Namespace,
) -> Dict[str, Any]:
    if args.dry_run:
        return {
            "pass": None,
            "status": "dry_run",
            "metric": f"corrected_permille_p{args.verdict_percentile * 100:.0f}",
            "metric_value": None,
            "tolerance_permille": args.corrected_tol,
            "calibration_valid_samples": 0,
            "measurement_valid_samples": 0,
            "offset_stability_pct": None,
            "offset_stability_warning": False,
            "calibration_requested_samples": args.calibration_n,
            "measurement_requested_samples": args.measurement_n,
            "calibration_completed_samples": len(cal_records),
            "measurement_completed_samples": len(meas_records),
            "partial": False,
            "shutdown_reason": SHUTDOWN_REASON,
            "min_measurement_cycles": args.min_measurement_cycles,
            "reasons": ["dry-run: commands were printed but PMCs were not executed"],
        }

    cal_valid = len(valid_records(cal_records))
    meas_valid = len(valid_records(meas_records))
    corrected_values = [
        float(record["corrected_permille"])
        for record in valid_records(meas_records)
        if record.get("corrected_permille") is not None
    ]
    corrected_quantile = percentile(corrected_values, args.verdict_percentile)
    measurement_counts = [
        float(record["max_count"])
        for record in valid_records(meas_records)
        if record.get("max_count") is not None
    ]
    measurement_max_count_median = statistics.median(measurement_counts) if measurement_counts else None

    cal_signed = [
        float(record["signed_delta"])
        for record in valid_records(cal_records)
        if record.get("signed_delta") is not None
    ]
    cal_signed_stdev = statistics.pstdev(cal_signed) if len(cal_signed) > 1 else 0.0
    offset_stability_pct = (
        abs(cal_signed_stdev / baseline_signed_offset) * 100.0
        if baseline_signed_offset else None
    )

    reasons: List[str] = []
    pass_verdict = True
    calibration_complete = phase_complete(cal_records, args.calibration_n)
    measurement_complete = phase_complete(meas_records, args.measurement_n)
    partial = bool(SHUTDOWN_REASON or not calibration_complete or not measurement_complete)
    if not calibration_complete:
        pass_verdict = False
        reasons.append(
            f"calibration completed {len(cal_records)}/{args.calibration_n} requested samples"
        )
    if not measurement_complete:
        pass_verdict = False
        reasons.append(
            f"measurement completed {len(meas_records)}/{args.measurement_n} requested samples"
        )
    if SHUTDOWN_REASON:
        pass_verdict = False
        reasons.append(f"run stopped before a complete verdict: {SHUTDOWN_REASON}")
    if cal_valid < args.min_valid_samples:
        pass_verdict = False
        reasons.append(f"calibration has {cal_valid} valid samples; need {args.min_valid_samples}")
    if meas_valid < args.min_valid_samples:
        pass_verdict = False
        reasons.append(f"measurement has {meas_valid} valid samples; need {args.min_valid_samples}")
    if corrected_quantile is None:
        pass_verdict = False
        reasons.append("no corrected measurement samples")
    if measurement_max_count_median is None:
        pass_verdict = False
        reasons.append("no valid measurement max_count samples")
    elif measurement_max_count_median < args.min_measurement_cycles:
        pass_verdict = False
        reasons.append(
            f"measurement median max_count {measurement_max_count_median:.0f} cycles "
            f"< minimum {args.min_measurement_cycles} cycles"
        )
    if corrected_quantile is not None and corrected_quantile > args.corrected_tol:
        pass_verdict = False
        reasons.append(
            f"corrected p{args.verdict_percentile * 100:.0f}={corrected_quantile:.6f}‰ "
            f"> tolerance {args.corrected_tol:.6f}‰"
        )

    stability_warning = bool(
        offset_stability_pct is not None
        and offset_stability_pct >= args.offset_stability_warn_pct
    )
    if stability_warning:
        reasons.append(
            f"calibration signed-offset stdev is {offset_stability_pct:.2f}% of median "
            f"(warning threshold {args.offset_stability_warn_pct:.2f}%)"
        )

    return {
        "pass": pass_verdict,
        "status": "pass" if pass_verdict else "fail",
        "metric": f"corrected_permille_p{args.verdict_percentile * 100:.0f}",
        "metric_value": corrected_quantile,
        "tolerance_permille": args.corrected_tol,
        "calibration_valid_samples": cal_valid,
        "measurement_valid_samples": meas_valid,
        "calibration_requested_samples": args.calibration_n,
        "measurement_requested_samples": args.measurement_n,
        "calibration_completed_samples": len(cal_records),
        "measurement_completed_samples": len(meas_records),
        "partial": partial,
        "shutdown_reason": SHUTDOWN_REASON,
        "min_measurement_cycles": args.min_measurement_cycles,
        "measurement_max_count_median": measurement_max_count_median,
        "offset_stability_pct": offset_stability_pct,
        "offset_stability_warning": stability_warning,
        "reasons": reasons,
    }


def fmt(value: Any, digits: int = 3) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def fmt_count(value: Any) -> str:
    if value is None:
        return "n/a"
    try:
        number = float(value)
    except (TypeError, ValueError):
        return str(value)
    if number >= 1_000_000_000:
        return f"{number / 1_000_000_000:.2f}G"
    if number >= 1_000_000:
        return f"{number / 1_000_000:.2f}M"
    if number >= 1_000:
        return f"{number / 1_000:.1f}K"
    return f"{number:.0f}"


def fmt_signed_count(value: Any) -> str:
    if value is None:
        return "n/a"
    try:
        number = float(value)
    except (TypeError, ValueError):
        return str(value)
    sign = "+" if number >= 0 else "-"
    return f"{sign}{fmt_count(abs(number))}"


def fmt_duration(seconds: Optional[float]) -> str:
    if seconds is None or not math.isfinite(float(seconds)):
        return "--:--"
    seconds = max(0, int(seconds))
    hours, rem = divmod(seconds, 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours:d}:{minutes:02d}:{secs:02d}"
    return f"{minutes:02d}:{secs:02d}"


def progress_bar(done: int, total: int, width: int = 18) -> str:
    if total <= 0:
        return "-" * width
    filled = int(width * min(done, total) / total)
    return "█" * filled + "░" * (width - filled)


class LiveDashboard:
    """Single-line PMC progress on stderr plus permanent checkpoint INFO logs.

    Status line is rewritten in place with CR+EL (no alternate screen, no
    cursor-up math, no flashing).  Permanent checkpoints emit through the
    normal logger so scroll-back keeps history that survives the run.
    """

    SPINNER = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
    MIN_WIDTH = 80
    TICK_SECONDS = 0.5
    TREND_WINDOW = 10
    WARMUP_VALID = 20
    TREND_RELATIVE = 0.05

    def __init__(self, args: argparse.Namespace) -> None:
        cols = shutil.get_terminal_size((120, 24)).columns
        self.enabled = bool(
            TERMINAL.live_graph_enabled
            and not args.quiet
            and cols >= self.MIN_WIDTH
        )
        self.tolerance = float(args.corrected_tol)
        self.deadline_mono: Optional[float] = getattr(args, "deadline", None)
        self.lock = threading.Lock()
        self.output_lock = threading.Lock()
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.has_line = False
        self.cursor_hidden = False
        self.phase = "idle"
        self.records: Sequence[Dict[str, Any]] = []
        self.requested = 0
        self.iteration = 0
        self.baseline_signed_offset: Optional[float] = None
        self.phase_started_mono = time.monotonic()
        self.probe_started_mono: Optional[float] = None
        self.last_probe_seconds: Optional[float] = None
        self.frame = 0
        self.last_checkpoint_count = 0
        if self.enabled:
            TERMINAL.set_dashboard_hooks(self.suspend_for_log, self._resume_after_log)

    def _resume_after_log(self) -> None:
        self._render()

    def _hide_cursor_locked(self) -> None:
        if self.enabled and not self.cursor_hidden:
            sys.stderr.write("\033[?25l")
            self.cursor_hidden = True

    def _show_cursor_locked(self) -> None:
        if self.cursor_hidden:
            sys.stderr.write("\033[?25h")
            self.cursor_hidden = False

    def _clear_line_locked(self) -> None:
        if self.has_line:
            sys.stderr.write("\r\033[2K")
            self.has_line = False

    def suspend_for_log(self) -> None:
        if not self.enabled:
            return
        with self.output_lock:
            self._clear_line_locked()
            sys.stderr.flush()

    def start_probe(
        self,
        *,
        phase: str,
        records: Sequence[Dict[str, Any]],
        requested: int,
        iteration: int,
        baseline_signed_offset: Optional[float] = None,
    ) -> None:
        if not self.enabled:
            return
        with self.output_lock:
            self._hide_cursor_locked()
            sys.stderr.flush()
        with self.lock:
            if self.phase != phase:
                self.phase_started_mono = time.monotonic()
                self.last_checkpoint_count = 0
                self.last_probe_seconds = None
            self.phase = phase
            self.records = records
            self.requested = requested
            self.iteration = iteration
            self.baseline_signed_offset = baseline_signed_offset
            self.probe_started_mono = time.monotonic()
            if self.thread is None or not self.thread.is_alive():
                self.stop_event.clear()
                self.thread = threading.Thread(target=self._tick, daemon=True)
                self.thread.start()
        self._render()

    def finish_probe(
        self,
        *,
        records: Sequence[Dict[str, Any]],
        baseline_signed_offset: Optional[float] = None,
    ) -> None:
        if not self.enabled:
            return
        now = time.monotonic()
        with self.lock:
            self.records = records
            self.baseline_signed_offset = baseline_signed_offset
            if self.probe_started_mono is not None:
                self.last_probe_seconds = now - self.probe_started_mono
            self.probe_started_mono = None
        self._maybe_emit_checkpoint()
        self._render()

    def stop(self) -> None:
        if not self.enabled:
            return
        TERMINAL.set_dashboard_hooks(None, None)
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join(timeout=1.0)
            self.thread = None
        with self.output_lock:
            self._clear_line_locked()
            self._show_cursor_locked()
            sys.stderr.flush()

    def _tick(self) -> None:
        while not self.stop_event.wait(self.TICK_SECONDS):
            self._render()

    def _trend(self, values: Sequence[float]) -> str:
        window = self.TREND_WINDOW
        if len(values) < window * 2:
            return "→"
        recent = statistics.median(values[-window:])
        prior = statistics.median(values[-2 * window:-window])
        if prior == 0:
            return "→"
        delta = (recent - prior) / abs(prior)
        if delta > self.TREND_RELATIVE:
            return "↑"
        if delta < -self.TREND_RELATIVE:
            return "↓"
        return "→"

    def _pace(self, p95: Optional[float], valid_n: int) -> Tuple[str, str]:
        if valid_n < self.WARMUP_VALID or p95 is None or self.tolerance <= 0:
            return "WARMUP", "dim"
        if p95 <= self.tolerance:
            return "PASS", "green"
        if p95 <= self.tolerance * 2.0:
            return "MARGIN", "yellow"
        return "FAIL", "red"

    def _budget_remaining(self) -> Optional[float]:
        if self.deadline_mono is None:
            return None
        return max(0.0, self.deadline_mono - time.monotonic())

    def _snapshot(self) -> Dict[str, Any]:
        with self.lock:
            self.frame += 1
            snap = {
                "phase": self.phase,
                "records": list(self.records),
                "requested": self.requested,
                "iteration": self.iteration,
                "baseline": self.baseline_signed_offset,
                "probe_started": self.probe_started_mono,
                "phase_started": self.phase_started_mono,
                "last_probe": self.last_probe_seconds,
                "spinner": self.SPINNER[self.frame % len(self.SPINNER)],
            }
        return snap

    def _phase_tail(
        self,
        snap: Dict[str, Any],
        valid: Sequence[Dict[str, Any]],
    ) -> Tuple[str, str, str]:
        if snap["phase"] == "calibration" or snap["baseline"] is None:
            signed_values = [
                float(record["signed_delta"]) for record in valid
                if record.get("signed_delta") is not None
            ]
            if signed_values:
                median = statistics.median(signed_values)
                stdev = (
                    statistics.pstdev(signed_values)
                    if len(signed_values) > 1
                    else 0.0
                )
                if median:
                    stable = stdev / abs(median) * 100.0
                    stable_text = f"{stable:5.1f}%"
                else:
                    stable_text = "  n/a"
                tail = (
                    f"baseline={median:+.0f}c "
                    f"stdev={stdev:.0f}c "
                    f"stable={stable_text}"
                )
            else:
                tail = "baseline=forming"
            return tail, "", "cyan"

        corrected = [
            float(record["corrected_permille"]) for record in valid
            if record.get("corrected_permille") is not None
        ]
        if corrected:
            p95 = percentile(corrected, 0.95)
            med = statistics.median(corrected)
            trend = self._trend(corrected)
            pace, pace_color = self._pace(p95, len(corrected))
            tail = (
                f"p95={p95:.3f}‰/{self.tolerance:.3f}‰ "
                f"med={med:.3f}‰ trend={trend}"
            )
            return tail, f" PACE: {pace}", pace_color

        return "corrected=n/a", "", "dim"

    def _compose(self, snap: Dict[str, Any]) -> Tuple[str, str]:
        now = time.monotonic()
        valid = valid_records(snap["records"])
        completed = len(snap["records"])
        requested = snap["requested"]
        pct = (completed / requested * 100.0) if requested else 0.0
        elapsed = now - snap["phase_started"]
        rate = (completed / elapsed) if elapsed > 0 and completed else 0.0
        eta = ((requested - completed) / rate) if rate > 0 else None
        budget = self._budget_remaining()

        probe_active = snap["probe_started"] is not None
        if probe_active:
            probe_age = now - snap["probe_started"]
        else:
            probe_age = snap["last_probe"]
        probe_status = "run " if probe_active else "idle"
        probe_dur = fmt_duration(probe_age) if probe_age is not None else "--:--"

        tag = f"[{snap['phase'].upper()[:4]:<4}]"
        bar = progress_bar(completed, requested)
        head = (
            f"{tag} {completed:>5}/{requested:<5} "
            f"[{bar}] {pct:5.1f}% "
            f"elapsed {fmt_duration(elapsed)} "
            f"eta {fmt_duration(eta)} "
            f"budget {fmt_duration(budget)}"
        )

        tail, pace_text, pace_color = self._phase_tail(snap, valid)
        valid_text = f"valid {len(valid):>3}/{completed:<3}"
        suffix = f"{valid_text} probe={probe_status} {probe_dur} {snap['spinner']}"

        plain = f"{head} │ {tail}{pace_text} │ {suffix}"
        ansi = (
            f"{TERMINAL.colorize(tag, 'bold')} "
            f"{completed:>5}/{requested:<5} "
            f"[{TERMINAL.colorize(bar, 'cyan')}] {pct:5.1f}% "
            f"elapsed {fmt_duration(elapsed)} "
            f"eta {fmt_duration(eta)} "
            f"budget {fmt_duration(budget)} "
            f"│ {tail}"
            f"{TERMINAL.colorize(pace_text, pace_color) if pace_text else ''}"
            f" │ {valid_text} probe={probe_status} {probe_dur} "
            f"{TERMINAL.colorize(snap['spinner'], 'cyan')}"
        )
        return plain, ansi

    def _render(self) -> None:
        if not self.enabled or self.requested == 0:
            return
        snap = self._snapshot()
        plain, ansi = self._compose(snap)
        cols = shutil.get_terminal_size((120, 24)).columns
        if len(plain) >= cols:
            ansi = plain[: cols - 1]
        with self.output_lock:
            self._hide_cursor_locked()
            sys.stderr.write("\r\033[2K" + ansi)
            sys.stderr.flush()
            self.has_line = True

    def _maybe_emit_checkpoint(self) -> None:
        with self.lock:
            requested = self.requested
            phase = self.phase
            records = list(self.records)
            baseline = self.baseline_signed_offset
            completed = len(records)
            previous = self.last_checkpoint_count
        if requested <= 0:
            return
        interval = max(20, requested // 20)
        if completed - previous < interval and completed != requested:
            return
        with self.lock:
            self.last_checkpoint_count = completed
        valid = valid_records(records)
        if phase == "calibration" or baseline is None:
            signed_values = [
                float(record["signed_delta"]) for record in valid
                if record.get("signed_delta") is not None
            ]
            if signed_values:
                median = statistics.median(signed_values)
                stdev = (
                    statistics.pstdev(signed_values)
                    if len(signed_values) > 1
                    else 0.0
                )
                stable_text = (
                    f"{stdev / abs(median) * 100.0:.1f}%" if median else "n/a"
                )
                message = (
                    f"CHK cal {completed}/{requested} "
                    f"valid={len(valid)}/{completed} "
                    f"baseline={median:+.0f}c stdev={stdev:.0f}c "
                    f"stable={stable_text}"
                )
            else:
                message = (
                    f"CHK cal {completed}/{requested} "
                    f"valid={len(valid)}/{completed} baseline=forming"
                )
        else:
            corrected = [
                float(record["corrected_permille"]) for record in valid
                if record.get("corrected_permille") is not None
            ]
            if corrected:
                p95 = percentile(corrected, 0.95)
                med = statistics.median(corrected)
                pace, _ = self._pace(p95, len(corrected))
                message = (
                    f"CHK meas {completed}/{requested} "
                    f"valid={len(valid)}/{completed} "
                    f"p95={p95:.4f}‰ med={med:.4f}‰ pace={pace}"
                )
            else:
                message = (
                    f"CHK meas {completed}/{requested} "
                    f"valid={len(valid)}/{completed} corrected=n/a"
                )
        log_info(message)


def print_results(
    cal_records: Sequence[Dict[str, Any]],
    meas_records: Sequence[Dict[str, Any]],
    baseline_abs_offset: float,
    baseline_signed_offset: float,
    verdict: Dict[str, Any],
    args: argparse.Namespace,
) -> None:
    cal = phase_stats(cal_records)
    meas = phase_stats(meas_records)
    corr = corrected_stats(meas_records)

    log_info("=" * 72)
    log_info("Canonical calibrated AMD PMC grouping skew results")
    log_info("=" * 72)
    log_info(f"event: FreeBSD={EVENT} silicon={SILICON_EVENT}")
    log_info(
        "correction: abs(measured_signed_delta - baseline_signed_offset) / "
        "max(last_a,last_b) * 1000"
    )

    log_info("")
    log_info("--- Phase 1: calibration (workload: true) ---")
    log_info(
        f"valid={cal['valid']}/{cal['records']} "
        f"b>a={cal['b_gt_a']} ({fmt(cal['pct_b_gt_a'], 1)}%)"
    )
    if cal["delta_abs"].get("n"):
        delta = cal["delta_abs"]
        signed = cal["signed_delta"]
        log_info(
            f"delta_abs cycles: min={delta['min']:.0f} mean={delta['mean']:.0f} "
            f"median={delta['median']:.0f} p95={delta['p95']:.0f} max={delta['max']:.0f}"
        )
        log_info(
            f"signed_delta cycles: median={signed['median']:.0f} "
            f"stdev={signed['stdev']:.0f}"
        )
    log_info(f"baseline_abs_offset:    {baseline_abs_offset:.0f} cycles")
    log_info(f"baseline_signed_offset: {baseline_signed_offset:.0f} cycles")

    log_info("")
    log_info(f"--- Phase 2: measurement (workload: dd count={args.dd_count:,}) ---")
    log_info(
        f"valid={meas['valid']}/{meas['records']} "
        f"b>a={meas['b_gt_a']} ({fmt(meas['pct_b_gt_a'], 1)}%)"
    )
    if meas["max_count"].get("n"):
        cycles = meas["max_count"]
        log_info(
            f"max_count cycles: median={cycles['median']:.0f} "
            f"({cycles['median'] / 1_000_000:.1f} M)"
        )
    if meas["permille"].get("n"):
        raw = meas["permille"]
        log_info(
            f"raw_permille: median={raw['median']:.6f}‰ "
            f"p95={raw['p95']:.6f}‰ max={raw['max']:.6f}‰"
        )

    log_info("")
    log_info("--- Phase 3: corrected metrics ---")
    corrected_delta = corr["corrected_delta"]
    corrected_permille = corr["corrected_permille"]
    corrected_signed = corr["corrected_signed_delta"]
    if corrected_delta.get("n"):
        log_info(
            f"corrected_delta cycles: median={corrected_delta['median']:.1f} "
            f"p95={corrected_delta['p95']:.1f} max={corrected_delta['max']:.1f}"
        )
    if corrected_permille.get("n"):
        log_info(
            f"corrected_permille: median={corrected_permille['median']:.6f}‰ "
            f"p95={corrected_permille['p95']:.6f}‰ max={corrected_permille['max']:.6f}‰"
        )
    if corrected_signed.get("n") and meas["max_count"].get("median"):
        noise_permille = corrected_signed["stdev"] / meas["max_count"]["median"] * 1000.0
        log_info(
            f"signed residual stdev: {corrected_signed['stdev']:.1f} cycles "
            f"({noise_permille:.6f}‰ of median max_count)"
        )

    log_info("")
    log_info("--- Verdict ---")
    metric_value = verdict.get("metric_value")
    if verdict.get("status") == "dry_run":
        log_info("DRY-RUN: commands were printed; no PMC samples were collected")
    elif verdict["pass"]:
        log_info(
            f"PASS: {verdict['metric']}={fmt(metric_value, 6)}‰ <= "
            f"{args.corrected_tol:.6f}‰"
        )
        log_info("duplicate PMCs agree after deterministic signed-offset calibration")
    else:
        log_warn(
            f"FAIL: {verdict['metric']}={fmt(metric_value, 6)}‰; "
            f"tolerance={args.corrected_tol:.6f}‰"
        )
    for reason in verdict.get("reasons", []):
        if verdict.get("status") == "dry_run":
            log_info(reason)
        elif not verdict["pass"] or "warning" in reason:
            log_warn(reason)
        else:
            log_info(reason)


def build_summary(
    *,
    run_id: str,
    started_at: str,
    platform_info: Dict[str, Any],
    hwpmc_status: Dict[str, Any],
    event_availability: Dict[str, bool],
    cal_records: Sequence[Dict[str, Any]],
    meas_records: Sequence[Dict[str, Any]],
    baseline_abs_offset: float,
    baseline_signed_offset: float,
    verdict: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "producer": PRODUCER,
        "run_id": run_id,
        "started_at": started_at,
        "ended_at": utc_now(),
        "event": {
            "freebsd_name": EVENT,
            "silicon_event": SILICON_EVENT,
            "description": "AMD core cycles not in halt",
        },
        "configuration": {
            "calibration_n": args.calibration_n,
            "measurement_n": args.measurement_n,
            "dd_count": args.dd_count,
            "min_valid_samples": args.min_valid_samples,
            "min_measurement_cycles": args.min_measurement_cycles,
            "corrected_tol_permille": args.corrected_tol,
            "verdict_percentile": args.verdict_percentile,
            "offset_stability_warn_pct": args.offset_stability_warn_pct,
            "minutes": args.minutes,
            "use_sudo": args.use_sudo,
            "sudo_non_interactive": args.sudo_non_interactive,
            "dry_run": args.dry_run,
        },
        "platform": platform_info,
        "hwpmc": hwpmc_status,
        "event_availability": event_availability,
        "shutdown_reason": SHUTDOWN_REASON,
        "partial": bool(
            SHUTDOWN_REASON
            or len(cal_records) < args.calibration_n
            or len(meas_records) < args.measurement_n
        ),
        "requested_samples": {
            "calibration": args.calibration_n,
            "measurement": args.measurement_n,
        },
        "completed_samples": {
            "calibration": len(cal_records),
            "measurement": len(meas_records),
        },
        "correction": {
            "method": "signed median calibration",
            "formula": "abs(measured_signed_delta - baseline_signed_offset) / max(last_a,last_b) * 1000",
            "baseline_abs_offset_cycles": baseline_abs_offset,
            "baseline_signed_offset_cycles": baseline_signed_offset,
        },
        "calibration": phase_stats(cal_records),
        "measurement": phase_stats(meas_records),
        "corrected": corrected_stats(meas_records),
        "verdict": verdict,
    }


CSV_FIELDS = [
    "run_id", "phase", "iteration", "workload", "started_at", "ended_at",
    "elapsed_seconds", "returncode", "command_outcome", "timed_out",
    "terminated_by_signal", "valid", "row_count", "last_a", "last_b",
    "max_count", "delta_abs", "signed_delta", "permille",
    "baseline_signed_offset", "corrected_signed_delta", "corrected_delta",
    "corrected_permille", "b_gt_a", "a_gt_b", "equal", "output_file",
    "raw_sha256", "stderr_tail",
]


def write_csv_atomic(path: Path, records: Sequence[Dict[str, Any]]) -> None:
    def write_rows(fp: Any) -> None:
        writer = csv.DictWriter(fp, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for record in records:
            row = {field: record.get(field) for field in CSV_FIELDS}
            row["workload"] = shlex.join(record.get("workload") or [])
            writer.writerow(row)

    atomic_write_file(path, write_rows, suffix="csv")


def write_outputs(
    outdir: Path,
    summary: Dict[str, Any],
    records: Sequence[Dict[str, Any]],
) -> None:
    json_path = outdir / "pmc-grouping-skew.json"
    csv_path = outdir / "pmc-grouping-skew.csv"
    payload = {
        **summary,
        "records_total": len(records),
        "records": list(records),
    }
    atomic_write_text(json_path, json.dumps(payload, indent=2, sort_keys=True, default=str))
    write_csv_atomic(csv_path, records)
    log_info(f"JSON: {json_path}")
    log_info(f"CSV:  {csv_path}")


def signal_handler(signum: int, _frame: Any) -> None:
    global SHUTDOWN_REASON, STOP_REQUESTED
    STOP_REQUESTED = True
    SHUTDOWN_REASON = f"signal {signum}"
    COMMAND_RUNNER.terminate_active(signal.SIGTERM)
    if ACTIVE_DASHBOARD is not None:
        ACTIVE_DASHBOARD.suspend_for_log()
    log_warn(f"received signal {signum}; stopping after the active probe")


def shutdown_exit_code() -> int:
    if SHUTDOWN_REASON is None:
        return 0
    match = re.fullmatch(r"signal (\d+)", SHUTDOWN_REASON)
    return 128 + int(match.group(1)) if match else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Collect calibrated FreeBSD AMD pmcstat duplicate-PMC skew data.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_collect.py --minutes 30\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_collect.py --calibration-n 30 --measurement-n 30\n"
            "  python3 py-scripts/pmc_grouping_skew_collect.py --dry-run --once\n"
        ),
    )
    parser.add_argument("--calibration-n", metavar="N",
        type=lambda value: parse_positive_int(value, "calibration-n"),
        default=DEFAULT_CALIBRATION_ITERATIONS,
        help="number of true-workload calibration probes")
    parser.add_argument("--measurement-n", metavar="N",
        type=lambda value: parse_positive_int(value, "measurement-n"),
        default=DEFAULT_MEASUREMENT_ITERATIONS,
        help="number of CPU-bound measurement probes")
    parser.add_argument("--dd-count", metavar="N",
        type=lambda value: parse_positive_int(value, "dd-count"),
        default=DEFAULT_DD_COUNT,
        help="dd count= value for the measurement workload")
    parser.add_argument("--min-valid-samples", metavar="N",
        type=lambda value: parse_positive_int(value, "min-valid-samples"),
        default=DEFAULT_MIN_VALID_SAMPLES,
        help="minimum valid samples required per phase for a pass verdict")
    parser.add_argument("--min-measurement-cycles", metavar="N",
        type=lambda value: parse_positive_int(value, "min-measurement-cycles"),
        default=DEFAULT_MIN_MEASUREMENT_CYCLES,
        help="minimum median measurement max_count cycles required for PASS")
    parser.add_argument("--corrected-tol", metavar="PERMILLE",
        type=lambda value: parse_nonnegative_float(value, "corrected-tol"),
        default=DEFAULT_CORRECTED_TOL_PERMILLE,
        help="corrected permille tolerance for the verdict percentile")
    parser.add_argument("--verdict-percentile", metavar="FRACTION",
        type=lambda value: parse_fraction(value, "verdict-percentile"),
        default=DEFAULT_VERDICT_PERCENTILE,
        help="corrected_permille percentile used for PASS/FAIL")
    parser.add_argument("--offset-stability-warn-pct", metavar="PCT",
        type=lambda value: parse_nonnegative_float(value, "offset-stability-warn-pct"),
        default=DEFAULT_OFFSET_STABILITY_WARN_PCT,
        help="warn if calibration signed-offset stdev exceeds this percentage of median")
    parser.add_argument("--outdir", type=Path, default=Path.cwd() / "pmu-skew-data",
        help="output directory for JSON, CSV, and raw pmcstat files")
    parser.add_argument("--minutes", metavar="M",
        type=lambda value: parse_nonnegative_float(value, "minutes"),
        default=DEFAULT_MINUTES,
        help="time budget in minutes; 0 means no deadline")
    parser.add_argument("--once", action="store_true",
        help="compatibility flag; this canonical collector always runs one calibrated pass")

    parser.add_argument("--sudo-cmd", default="sudo", help="sudo binary")
    parser.add_argument("--sudo-interactive", dest="sudo_non_interactive",
        action="store_false", help="allow interactive sudo prompts")
    parser.add_argument("--no-sudo", dest="use_sudo", action="store_false",
        help="do not prefix privileged commands with sudo when not root")
    parser.set_defaults(use_sudo=True, sudo_non_interactive=True)

    parser.add_argument("--preflight-timeout", metavar="S",
        type=lambda value: parse_positive_int(value, "preflight-timeout"),
        default=DEFAULT_PREFLIGHT_TIMEOUT,
        help="maximum seconds for setup/probe commands")
    parser.add_argument("--command-timeout-overhead", metavar="S",
        type=lambda value: parse_positive_int(value, "command-timeout-overhead"),
        default=DEFAULT_COMMAND_TIMEOUT_OVERHEAD,
        help="seconds added to each workload timeout")
    parser.add_argument("--command-grace-seconds", metavar="S",
        type=lambda value: parse_positive_int(value, "command-grace-seconds"),
        default=DEFAULT_COMMAND_GRACE_SECONDS,
        help="SIGTERM grace period before SIGKILL")

    parser.add_argument("--color", choices=("auto", "always", "never"), default="auto",
        help="ANSI color mode")
    parser.add_argument("--live-graph", choices=("auto", "always", "never"), default="auto",
        help="show live progress for active pmcstat probes")
    parser.add_argument("-v", "--verbose", action="store_true",
        help="print command and per-probe diagnostics")
    parser.add_argument("--quiet", action="store_true",
        help="suppress live progress when possible")
    parser.add_argument("--dry-run", action="store_true",
        help="print command shapes and write empty output skeleton")
    parser.add_argument("--no-hwpmc-load", action="store_true",
        help="do not attempt to kldload hwpmc before collection")
    parser.add_argument("--allow-non-freebsd", action="store_true",
        help="allow execution on non-FreeBSD hosts for command-shape inspection")
    parser.add_argument("--allow-non-amd", action="store_true",
        help="allow execution when kern.hwpmc.cpuid is not AuthenticAMD")
    parser.add_argument("--allow-unknown-generation", action="store_true",
        help="allow unknown/future AMD generation after manual PPR verification")
    return parser


def main() -> int:
    global ACTIVE_DASHBOARD
    args = build_parser().parse_args()
    configure_terminal(args)
    args.outdir = args.outdir.expanduser().resolve()
    args.deadline = None if args.minutes == 0 else time.monotonic() + args.minutes * 60.0
    resolve_tool_paths(args)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    if not args.dry_run:
        check_tool("pmcstat")
        if args.use_sudo and not is_root():
            check_tool(args.sudo_cmd)
            sudo_check = COMMAND_RUNNER.run(
                [args.sudo_cmd, "-n", "true"] if args.sudo_non_interactive else [args.sudo_cmd, "true"],
                timeout_seconds=args.preflight_timeout,
                grace_seconds=args.command_grace_seconds,
                inherit_stdin=not args.sudo_non_interactive,
            )
            if sudo_check.get("returncode") != 0:
                die("sudo preflight failed; run as root or pass --sudo-interactive")
        elif not is_root() and not args.use_sudo:
            die("pmcstat requires root. Run as root or remove --no-sudo.")

    args.outdir.mkdir(parents=True, exist_ok=True)
    raw_dir = args.outdir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    started_at = utc_now()
    run_id = uuid.uuid4().hex
    hwpmc_status = ensure_hwpmc_loaded(args)
    if hwpmc_status.get("error") and not args.dry_run and platform.system() == "FreeBSD":
        die(f"hwpmc unavailable: {hwpmc_status['error']}")

    platform_info = collect_platform_info(args)
    validate_platform(platform_info, args)
    cpuid = platform_info.get("parsed_cpuid", {})

    events = collect_event_list(args)
    event_availability = check_event_availability(events, [EVENT])
    if not event_availability.get(EVENT, False) and not args.dry_run:
        die(f"required pmcstat event not available: {EVENT}")

    log_info(
        "target CPU: "
        f"{cpuid.get('generation', 'unknown')} family={cpuid.get('family_hex')} "
        f"model={cpuid.get('model_hex')} stepping={cpuid.get('stepping')} "
        f"PPR={cpuid.get('ppr')}"
    )
    log_info(f"event: FreeBSD={EVENT} silicon={SILICON_EVENT}")
    log_info(f"output directory: {args.outdir}")
    log_info(f"calibration iterations: {args.calibration_n}")
    log_info(f"measurement iterations:  {args.measurement_n}")
    log_info(f"measurement workload:    dd if=/dev/zero of=/dev/null bs=4096 count={args.dd_count}")
    dashboard = LiveDashboard(args)
    ACTIVE_DASHBOARD = dashboard

    try:
        cal_records = run_phase(
            args,
            phase="calibration",
            iterations=args.calibration_n,
            workload=[args.true_cmd],
            raw_dir=raw_dir,
            run_id=run_id,
            timeout_hint=5.0,
            dashboard=dashboard,
        )
        baseline_abs_offset, baseline_signed_offset = calibration_offsets(cal_records)
        dashboard.finish_probe(
            records=cal_records,
            baseline_signed_offset=baseline_signed_offset,
        )
        meas_records = run_phase(
            args,
            phase="measurement",
            iterations=args.measurement_n,
            workload=[args.dd_cmd, "if=/dev/zero", "of=/dev/null", "bs=4096", f"count={args.dd_count}"],
            raw_dir=raw_dir,
            run_id=run_id,
            timeout_hint=90.0,
            dashboard=dashboard,
            baseline_signed_offset=baseline_signed_offset,
        )
        baseline_abs_offset, baseline_signed_offset = apply_correction(cal_records, meas_records)
        verdict = verdict_from_stats(cal_records, meas_records, baseline_signed_offset, args)
        all_records = cal_records + meas_records
    finally:
        dashboard.stop()
        ACTIVE_DASHBOARD = None

    print_results(
        cal_records,
        meas_records,
        baseline_abs_offset,
        baseline_signed_offset,
        verdict,
        args,
    )
    summary = build_summary(
        run_id=run_id,
        started_at=started_at,
        platform_info=platform_info,
        hwpmc_status=hwpmc_status,
        event_availability=event_availability,
        cal_records=cal_records,
        meas_records=meas_records,
        baseline_abs_offset=baseline_abs_offset,
        baseline_signed_offset=baseline_signed_offset,
        verdict=verdict,
        args=args,
    )
    write_outputs(args.outdir, summary, all_records)
    return shutdown_exit_code()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   v4 collector — calibration subtraction + signed-delta noise analysis.
#
#   Background (from v3):
#     pmcstat -C arms two PMC rows via sequential pmc_start() syscalls.
#     This produces a constant ~199 K cycle arming offset (b > a in 94 % of runs).
#     With a sleep workload (~3.9 M cycles) this appears as ~50 permille.
#     With dd count=500000 (~467 M cycles) it falls to ~0.42 permille.
#
#   v4 approach:
#
#   PHASE 1 — calibration
#     N probes of: pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -- true
#     baseline_offset = median(calibration delta_abs)   expected ~199 K cycles
#
#   PHASE 2 — measurement
#     N probes of: pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc
#                   -- dd count=<N> if=/dev/zero of=/dev/null bs=4096
#     corrected_delta   = measured_delta_abs - baseline_offset
#     corrected_permille = corrected_delta / total_cycles * 1000
#
#   PHASE 3 — signed noise floor
#     signed_delta = last_b - last_a  (positive = b larger than a)
#     true_noise_stdev = pstdev(signed_delta) across measurement samples
#     This is the actual precision of the measurement, independent of bias.
#
#   Output:
#     JSON  — full records + per-phase summaries + corrected metrics
#     CSV   — flat record table (phase, iteration, last_a, last_b, delta_abs,
#              signed_delta, permille, corrected_delta, corrected_permille, b_gt_a)
#     Text  — printed results table and verdicts

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import signal
import shutil
import statistics
import sys
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

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
    atomic_write_text,
    fsync_directory,
    read_text_file,
    sha256_file,
    unique_tmp_path,
    utc_now,
)
from pmu_common.metrics import percentile
from pmu_common.terminal import Terminal


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SCHEMA_VERSION = 4
PRODUCER = "pmc_grouping_skew_v4_collect.py"
EVENT = "ls_not_halted_cyc"

DEFAULT_CALIBRATION_ITERATIONS = 50
DEFAULT_MEASUREMENT_ITERATIONS = 50
DEFAULT_DD_COUNT = 500_000
DEFAULT_COMMAND_TIMEOUT_OVERHEAD = 30
DEFAULT_COMMAND_GRACE_SECONDS = 10
DEFAULT_MINUTES = 30.0
DEFAULT_CORRECTED_TOL_PERMILLE = 1.0

STOP_REQUESTED = False
SHUTDOWN_REASON: Optional[str] = None
TERMINAL = Terminal("pmc-skew-v4")
COMMAND_RUNNER = CommandRunner(TERMINAL)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def log_info(m: str) -> None:    TERMINAL.info(m)
def log_warn(m: str) -> None:    TERMINAL.warn(m)
def log_err(m: str) -> None:     TERMINAL.error(m)
def log_verbose(m: str) -> None: TERMINAL.verbose(m)


def die(msg: str, code: int = 1) -> None:
    log_err(msg)
    raise SystemExit(code)


def sysctl_value(name: str) -> str:
    return simple_command_value(["sysctl", "-n", name])


def check_tool(name: str) -> Optional[str]:
    try:
        return common_check_tool(name, required=True)
    except FileNotFoundError:
        die(f"required tool not found in PATH: {name}")


def run_cmd(
    argv: Sequence[str],
    args: argparse.Namespace,
    *,
    timeout_s: Optional[float] = None,
    label: Optional[str] = None,
) -> Dict[str, Any]:
    return COMMAND_RUNNER.run(
        SudoConfig(args.use_sudo, args.sudo_cmd, args.sudo_non_interactive).apply(argv),
        dry_run=args.dry_run,
        timeout_seconds=timeout_s,
        grace_seconds=DEFAULT_COMMAND_GRACE_SECONDS,
        progress_label=label,
    )


def enough_time(args: argparse.Namespace, needed_s: float) -> bool:
    if args.dry_run:
        return True
    rem = remaining_seconds(args.deadline)
    return rem is None or rem > max(1.0, needed_s)


# ---------------------------------------------------------------------------
# pmcstat parsing
# ---------------------------------------------------------------------------

def parse_pmcstat_two_counter(text: str) -> Optional[Dict[str, Any]]:
    last_a = last_b = None
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[0].isdigit() and fields[1].isdigit():
            last_a, last_b = int(fields[0]), int(fields[1])

    if last_a is None or last_b is None:
        return None

    delta_abs = abs(last_a - last_b)
    signed_delta = last_b - last_a          # positive = b > a
    maximum = max(last_a, last_b)
    permille = (delta_abs * 1000.0 / maximum) if maximum > 0 else 0.0

    return {
        "last_a": last_a,
        "last_b": last_b,
        "delta_abs": delta_abs,
        "signed_delta": signed_delta,
        "permille": permille,
        "b_gt_a": last_b > last_a,
        "a_gt_b": last_a > last_b,
        "equal": last_a == last_b,
    }


# ---------------------------------------------------------------------------
# Single probe
# ---------------------------------------------------------------------------

def run_probe(
    args: argparse.Namespace,
    *,
    phase: str,
    iteration: int,
    workload: List[str],
    raw_dir: Path,
    run_id: str,
    timeout_hint: float = 60.0,
) -> Dict[str, Any]:
    label = f"{phase} iter={iteration}"
    out_name = f"{phase}-{iteration:04d}.out"
    output_file = raw_dir / out_name
    tmp_out = output_file if args.dry_run else unique_tmp_path(output_file, "out")

    command = [
        "pmcstat", "-C", "-q",
        "-p", EVENT,
        "-p", EVENT,
        "-o", str(tmp_out),
        "--",
    ] + workload

    timeout_s = bounded_timeout(
        args.deadline,
        timeout_hint + DEFAULT_COMMAND_TIMEOUT_OVERHEAD,
    )

    result = run_cmd(command, args, timeout_s=timeout_s, label=label)
    rc = result.get("returncode")

    if tmp_out.exists() and rc == 0:
        tmp_out.replace(output_file)
        fsync_directory(output_file.parent)
    elif not args.dry_run:
        for p in (tmp_out, output_file):
            try:
                p.unlink()
            except OSError:
                pass

    output_text = "" if args.dry_run else read_text_file(output_file)
    parsed = parse_pmcstat_two_counter(output_text) if output_text else None
    terminated_by_signal = result.get("terminated_by_signal")

    record: Dict[str, Any] = {
        "run_id": run_id,
        "phase": phase,
        "iteration": iteration,
        "workload": workload,
        "started_at": result.get("started_at"),
        "ended_at": result.get("ended_at"),
        "elapsed_seconds": result.get("elapsed_seconds"),
        "returncode": rc,
        "terminated_by_signal": terminated_by_signal,
        "command_outcome": result.get("outcome"),
        "last_a": parsed["last_a"] if parsed else None,
        "last_b": parsed["last_b"] if parsed else None,
        "delta_abs": parsed["delta_abs"] if parsed else None,
        "signed_delta": parsed["signed_delta"] if parsed else None,
        "permille": parsed["permille"] if parsed else None,
        "b_gt_a": parsed["b_gt_a"] if parsed else None,
        "a_gt_b": parsed["a_gt_b"] if parsed else None,
        "equal": parsed["equal"] if parsed else None,
        "valid": bool(parsed and rc == 0),
        # corrected fields filled in after calibration
        "corrected_delta": None,
        "corrected_permille": None,
        "output_file": str(output_file) if not args.dry_run else None,
        "raw_sha256": sha256_file(output_file) if (not args.dry_run and output_file.exists()) else None,
        "stderr_tail": (result.get("stderr") or "")[-400:],
    }
    return record


# ---------------------------------------------------------------------------
# Phase runners
# ---------------------------------------------------------------------------

def run_calibration(args: argparse.Namespace, run_id: str, raw_dir: Path) -> List[Dict[str, Any]]:
    """Phase 1: calibration probes with 'true' workload."""
    records = []
    workload = ["true"]
    log_info(f"Phase 1 — calibration: workload=true x{args.calibration_n} iterations")
    for it in range(1, args.calibration_n + 1):
        if STOP_REQUESTED:
            break
        if not enough_time(args, 5):
            log_warn(f"calibration: deadline reached at iter={it}")
            break
        rec = run_probe(args, phase="calibration", iteration=it, workload=workload,
                        raw_dir=raw_dir, run_id=run_id, timeout_hint=5.0)
        records.append(rec)
        if rec["valid"]:
            b_gt = "b>a" if rec["b_gt_a"] else ("a>b" if rec["a_gt_b"] else "eq")
            log_verbose(f"  cal it={it}: delta={rec['delta_abs']} signed={rec['signed_delta']} {b_gt}")
    return records


def run_measurement(args: argparse.Namespace, run_id: str, raw_dir: Path) -> List[Dict[str, Any]]:
    """Phase 2: measurement probes with dd count=N workload."""
    records = []
    workload = ["dd", "if=/dev/zero", "of=/dev/null", "bs=4096", f"count={args.dd_count}"]
    log_info(f"Phase 2 — measurement: workload=dd count={args.dd_count:,} x{args.measurement_n} iterations")
    for it in range(1, args.measurement_n + 1):
        if STOP_REQUESTED:
            break
        if not enough_time(args, 90):
            log_warn(f"measurement: deadline reached at iter={it}")
            break
        rec = run_probe(args, phase="measurement", iteration=it, workload=workload,
                        raw_dir=raw_dir, run_id=run_id, timeout_hint=90.0)
        records.append(rec)
        if rec["valid"]:
            b_gt = "b>a" if rec["b_gt_a"] else ("a>b" if rec["a_gt_b"] else "eq")
            log_verbose(f"  meas it={it}: delta={rec['delta_abs']} permille={rec['permille']:.3f}‰ {b_gt}")
    return records


# ---------------------------------------------------------------------------
# Corrected metrics
# ---------------------------------------------------------------------------

def apply_correction(
    cal_records: List[Dict[str, Any]],
    meas_records: List[Dict[str, Any]],
) -> Tuple[float, List[Dict[str, Any]]]:
    """
    Compute baseline_offset from calibration, apply to measurement records.
    Returns (baseline_offset, updated_meas_records).
    """
    cal_valid = [r for r in cal_records if r.get("valid")]
    if not cal_valid:
        return 0.0, meas_records

    deltas = [r["delta_abs"] for r in cal_valid if r["delta_abs"] is not None]
    baseline_offset = statistics.median(deltas) if deltas else 0.0

    for r in meas_records:
        if r.get("valid") and r["delta_abs"] is not None and r["last_a"] is not None:
            cd = max(0.0, r["delta_abs"] - baseline_offset)
            total = max(r["last_a"], r["last_b"])
            cp = (cd * 1000.0 / total) if total > 0 else 0.0
            r["corrected_delta"] = cd
            r["corrected_permille"] = cp

    return baseline_offset, meas_records


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def phase_stats(records: List[Dict[str, Any]]) -> Dict[str, Any]:
    valid = [r for r in records if r.get("valid")]
    n = len(valid)

    def s(lst: list) -> Dict[str, Any]:
        if not lst:
            return {"n": 0}
        return {
            "n": len(lst),
            "min": min(lst),
            "mean": statistics.fmean(lst),
            "median": statistics.median(lst),
            "p90": percentile(lst, 0.90),
            "p95": percentile(lst, 0.95),
            "max": max(lst),
            "stdev": statistics.pstdev(lst) if len(lst) > 1 else 0.0,
        }

    deltas = [r["delta_abs"] for r in valid if r["delta_abs"] is not None]
    signed = [r["signed_delta"] for r in valid if r["signed_delta"] is not None]
    perms = [r["permille"] for r in valid if r["permille"] is not None]
    last_as = [r["last_a"] for r in valid if r["last_a"] is not None]
    b_gt = sum(1 for r in valid if r.get("b_gt_a"))

    return {
        "n": n,
        "b_gt_a": b_gt,
        "pct_b_gt_a": (b_gt / n * 100) if n else None,
        "delta_abs": s(deltas),
        "signed_delta": s(signed),
        "permille": s(perms),
        "last_a": s(last_as),
    }


def corrected_stats(meas_records: List[Dict[str, Any]]) -> Dict[str, Any]:
    valid = [r for r in meas_records if r.get("valid") and r.get("corrected_permille") is not None]
    cp = [r["corrected_permille"] for r in valid]
    cd = [r["corrected_delta"] for r in valid]

    def s(lst: list) -> Dict[str, Any]:
        if not lst:
            return {"n": 0}
        return {
            "n": len(lst),
            "min": min(lst),
            "mean": statistics.fmean(lst),
            "median": statistics.median(lst),
            "p90": percentile(lst, 0.90),
            "p95": percentile(lst, 0.95),
            "max": max(lst),
            "stdev": statistics.pstdev(lst) if len(lst) > 1 else 0.0,
        }

    return {
        "corrected_permille": s(cp),
        "corrected_delta": s(cd),
    }


def print_results(
    cal_records: List[Dict[str, Any]],
    meas_records: List[Dict[str, Any]],
    baseline_offset: float,
    args: argparse.Namespace,
) -> None:
    cal_st = phase_stats(cal_records)
    meas_st = phase_stats(meas_records)
    corr_st = corrected_stats(meas_records)

    sep = "=" * 72
    log_info(sep)
    log_info("=== v4 calibration results ===")
    log_info(sep)

    log_info("")
    log_info("--- Phase 1: calibration (workload: true) ---")
    log_info(f"  n={cal_st['n']}  b>a={cal_st['b_gt_a']} ({cal_st['pct_b_gt_a']:.1f}%)")
    if cal_st["delta_abs"].get("n"):
        d = cal_st["delta_abs"]
        log_info(f"  delta_abs:     min={d['min']:.0f}  mean={d['mean']:.0f}  "
                 f"median={d['median']:.0f}  max={d['max']:.0f}  stdev={d['stdev']:.0f}")
    if cal_st["signed_delta"].get("n"):
        s = cal_st["signed_delta"]
        log_info(f"  signed_delta:  mean={s['mean']:.0f}  stdev={s['stdev']:.0f}  "
                 f"(stdev/median={s['stdev']/max(s['median'],1)*100:.1f}%)")
    log_info(f"  baseline_offset (median delta_abs): {baseline_offset:.0f} cycles")

    log_info("")
    log_info(f"--- Phase 2: measurement (workload: dd count={args.dd_count:,}) ---")
    log_info(f"  n={meas_st['n']}  b>a={meas_st['b_gt_a']} ({meas_st['pct_b_gt_a']:.1f}%)")
    if meas_st["last_a"].get("n"):
        log_info(f"  med_total_cycles: {meas_st['last_a']['median']:.0f}  "
                 f"({meas_st['last_a']['median']/1e6:.1f} M)")
    if meas_st["delta_abs"].get("n"):
        d = meas_st["delta_abs"]
        log_info(f"  delta_abs:     min={d['min']:.0f}  mean={d['mean']:.0f}  "
                 f"median={d['median']:.0f}  max={d['max']:.0f}  stdev={d['stdev']:.0f}")
    if meas_st["permille"].get("n"):
        p = meas_st["permille"]
        log_info(f"  raw_permille:  mean={p['mean']:.4f}  median={p['median']:.4f}  "
                 f"p95={p['p95']:.4f}  max={p['max']:.4f}")

    log_info("")
    log_info("--- Phase 3: corrected metrics ---")
    log_info(f"  baseline_offset:       {baseline_offset:.0f} cycles")
    if corr_st["corrected_delta"].get("n"):
        cd = corr_st["corrected_delta"]
        log_info(f"  corrected_delta:       median={cd['median']:.1f}  "
                 f"mean={cd['mean']:.1f}  stdev={cd['stdev']:.1f} cycles")
    if corr_st["corrected_permille"].get("n"):
        cp = corr_st["corrected_permille"]
        log_info(f"  corrected_permille:    median={cp['median']:.4f}‰  "
                 f"mean={cp['mean']:.4f}‰  p95={cp['p95']:.4f}‰  max={cp['max']:.4f}‰")
    if meas_st["signed_delta"].get("n"):
        s = meas_st["signed_delta"]
        log_info(f"  true_noise_stdev:      {s['stdev']:.0f} cycles  "
                 f"({s['stdev']/max(meas_st['last_a'].get('median',1),1)*1000:.4f}‰ of total cycles)")

    log_info("")
    log_info("--- Verdict ---")
    med_cp = corr_st["corrected_permille"].get("median", None) if corr_st["corrected_permille"].get("n") else None
    if med_cp is not None:
        if med_cp <= args.corrected_tol:
            log_info(f"  PASS: corrected_permille median={med_cp:.4f}‰ <= tolerance={args.corrected_tol}‰")
            log_info("  The two PMC counters are effectively identical once the arming offset is removed.")
        else:
            log_warn(f"  WARN: corrected_permille median={med_cp:.4f}‰ > tolerance={args.corrected_tol}‰")
            log_warn("  Some residual divergence remains after calibration subtraction.")
    else:
        log_warn("  insufficient data for verdict")


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def build_summary(
    cal_records: List[Dict[str, Any]],
    meas_records: List[Dict[str, Any]],
    baseline_offset: float,
    args: argparse.Namespace,
    run_id: str,
) -> Dict[str, Any]:
    return {
        "producer": PRODUCER,
        "schema_version": SCHEMA_VERSION,
        "run_id": run_id,
        "updated_at": utc_now(),
        "configuration": {
            "calibration_n": args.calibration_n,
            "measurement_n": args.measurement_n,
            "dd_count": args.dd_count,
            "corrected_tol_permille": args.corrected_tol,
        },
        "baseline_offset_cycles": baseline_offset,
        "calibration": phase_stats(cal_records),
        "measurement": phase_stats(meas_records),
        "corrected": corrected_stats(meas_records),
    }


def write_outputs(
    all_records: List[Dict[str, Any]],
    summary: Dict[str, Any],
    outdir: Path,
) -> None:
    json_path = outdir / "pmc-grouping-skew-v4.json"
    csv_path = outdir / "pmc-grouping-skew-v4.csv"

    payload = {**summary, "records_total": len(all_records)}
    atomic_write_text(json_path, json.dumps(payload, indent=2, default=str))
    fsync_directory(outdir)

    csv_fields = [
        "run_id", "phase", "iteration", "workload",
        "started_at", "ended_at", "elapsed_seconds", "returncode", "valid",
        "last_a", "last_b", "delta_abs", "signed_delta", "permille",
        "corrected_delta", "corrected_permille",
        "b_gt_a", "a_gt_b", "equal",
        "output_file", "raw_sha256",
    ]
    with csv_path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=csv_fields, extrasaction="ignore")
        w.writeheader()
        for r in all_records:
            row = {k: r.get(k) for k in csv_fields}
            row["workload"] = " ".join(r.get("workload") or [])
            w.writerow(row)
    fsync_directory(outdir)

    log_info(f"JSON: {json_path}")
    log_info(f"CSV:  {csv_path}")


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=PRODUCER,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--calibration-n", type=int, default=DEFAULT_CALIBRATION_ITERATIONS,
                   metavar="N", help="calibration iterations (true workload)")
    p.add_argument("--measurement-n", type=int, default=DEFAULT_MEASUREMENT_ITERATIONS,
                   metavar="N", help="measurement iterations (dd workload)")
    p.add_argument("--dd-count", type=int, default=DEFAULT_DD_COUNT,
                   metavar="N", help="dd count= for measurement workload")
    p.add_argument("--corrected-tol", type=float, default=DEFAULT_CORRECTED_TOL_PERMILLE,
                   metavar="PERMILLE", help="corrected_permille pass threshold")
    p.add_argument("--outdir", type=Path, default=Path("./pmu-skew-data-v4"),
                   metavar="DIR", help="output directory")
    p.add_argument("--minutes", type=float, default=DEFAULT_MINUTES,
                   metavar="M", help="time budget in minutes (0 = unlimited)")
    p.add_argument("--once", action="store_true",
                   help="run one pass then exit regardless of --minutes")
    p.add_argument("--dry-run", action="store_true", help="print commands, do not run")
    p.add_argument("--use-sudo", action="store_true", help="prefix commands with sudo")
    p.add_argument("--sudo-cmd", default="sudo", help="sudo binary")
    p.add_argument("--sudo-non-interactive", action="store_true")
    return p


# ---------------------------------------------------------------------------
# Signal handling
# ---------------------------------------------------------------------------

def _handle_signal(sig: int, _frame: Any) -> None:
    global STOP_REQUESTED, SHUTDOWN_REASON
    STOP_REQUESTED = True
    SHUTDOWN_REASON = f"signal {sig}"
    log_warn(f"shutdown requested ({SHUTDOWN_REASON})")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    global STOP_REQUESTED

    args = build_parser().parse_args()

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    if not is_root() and not args.use_sudo and not args.dry_run:
        die("pmcstat requires root. Run as root or pass --use-sudo.")

    args.outdir.mkdir(parents=True, exist_ok=True)
    raw_dir = args.outdir / "raw"
    raw_dir.mkdir(exist_ok=True)

    deadline = None
    if args.minutes > 0 and not args.once:
        deadline = time.monotonic() + args.minutes * 60
    args.deadline = deadline

    # Platform info
    try:
        cpuid_raw = sysctl_value("kern.hwpmc.cpuid")
        cpu_info = parse_hwpmc_cpuid(cpuid_raw)
    except Exception:
        cpu_info = {}

    log_info(f"CPU: {cpu_info.get('uarch', 'unknown')} "
             f"family={cpu_info.get('family_hex','?')} model={cpu_info.get('model_hex','?')}")
    log_info(f"calibration: {args.calibration_n} iters (true)")
    log_info(f"measurement: {args.measurement_n} iters (dd count={args.dd_count:,})")
    log_info(f"corrected tolerance: {args.corrected_tol} ‰")

    run_id = str(uuid.uuid4())

    log_info("")
    log_info("=== pass 1 ===")

    cal_records = run_calibration(args, run_id, raw_dir)
    meas_records = run_measurement(args, run_id, raw_dir)

    baseline_offset, meas_records = apply_correction(cal_records, meas_records)

    print_results(cal_records, meas_records, baseline_offset, args)

    all_records = cal_records + meas_records
    summary = build_summary(cal_records, meas_records, baseline_offset, args, run_id)

    platform_info = {
        "os": sys.platform,
        "cpu": cpu_info,
        "hostname": os.uname().nodename,
    }
    summary["platform"] = platform_info

    write_outputs(all_records, summary, args.outdir)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

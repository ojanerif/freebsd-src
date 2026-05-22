#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   v2 collector for AMD pmcstat grouping skew measurements.
#
#   This collector corresponds to pmcstat_grouping_test_v2.sh and exercises
#   the two structural fixes introduced in v2:
#
#     1. CPU-bound workload (cpuset -l 0 timeout 5 dd if=/dev/zero of=/dev/null)
#        instead of the idle `sleep N` used in v1.  The larger cycle accumulation
#        makes the absolute timing jitter between pmc_start() calls negligible.
#
#     2. CPU affinity pinning (cpuset -l 0) eliminates inter-core migration and
#        per-CCX frequency-scaling divergence.
#
#   Default workload:
#     cpuset -l 0 timeout 5 dd if=/dev/zero of=/dev/null bs=4096
#   Override with --workload-cmd.
#
#   Output schema is identical to pmc_grouping_skew_collect.py (v1): same JSON
#   and CSV structure, so results from both can be compared directly.
#
#   Key differences from v1:
#     - DEFAULT_TOLERANCE_PERMILLE = 50  (v1 = 100 for research, 250 in ATF)
#     - DEFAULT_WORKLOAD_CMD uses cpuset + dd
#     - No kyua integration (direct pmcstat probes only)
#     - Output dir default: ./pmu-skew-data-v2

from __future__ import annotations

import argparse
import copy
import csv
import json
import math
import os
import platform
import queue
import re
import signal
import shlex
import statistics
import shutil
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
    atomic_write_text,
    fsync_directory,
    read_text_file,
    sha256_file,
    tail_text,
    unique_tmp_path,
    utc_now,
)
from pmu_common.metrics import format_skew, percentile, permille_to_percent
from pmu_common.terminal import Terminal


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Empirical v1 (sleep 5) baseline on EPYC:
#   mean ~52‰, p95 ~104‰, max ~123‰, fail rate ~13% at tolerance=100
#
# Expected v2 (cpuset+dd) baseline:
#   mean <5‰, p95 <10‰, max <20‰ — timing jitter is negligible at high counts
DEFAULT_TOLERANCE_PERMILLE = 50

DEFAULT_WORKLOAD_CMD: List[str] = [
    "cpuset", "-l", "0",
    "timeout", "5",
    "dd", "if=/dev/zero", "of=/dev/null", "bs=4096",
]

# Minimum cycle count sanity check: CPU-bound dd should accumulate far more
# than an idle sleep.  Flag (but do not fail) if last_a is below this.
DEFAULT_MIN_CYCLE_COUNT = 10_000_000

DEFAULT_MINUTES = 120.0
DEFAULT_COMMAND_GRACE_SECONDS = 10
DEFAULT_COMMAND_TIMEOUT_SECONDS = 60
SCHEMA_VERSION = 3
PRODUCER = "pmc_grouping_skew_v2_collect.py"

PMSTAT_DUPLICATE_EVENT = "ls_not_halted_cyc"
PMSTAT_INSTRUCTION_EVENT = "ex_ret_instr"

SKEW_RE = re.compile(
    r"delta=(?P<delta>[0-9]+(?:\.[0-9]+)?)\s+"
    r"max=(?P<max>[0-9]+(?:\.[0-9]+)?)\s+"
    r"permille=(?P<permille>[0-9]+(?:\.[0-9]+)?)"
)

STOP_REQUESTED = False
SHUTDOWN_REASON: Optional[str] = None
TERMINAL = Terminal("pmc-grouping-skew-v2")
COMMAND_RUNNER = CommandRunner(TERMINAL)


# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------

def colorize(text: str, color: str) -> str:
    return TERMINAL.colorize(text, color)


def configure_terminal(args: argparse.Namespace) -> None:
    TERMINAL.configure(
        color=args.color,
        live_graph=False,
        verbose=args.verbose,
        quiet=args.quiet,
    )


def log_info(message: str) -> None:
    TERMINAL.info(message)


def log_warn(message: str) -> None:
    TERMINAL.warn(message)


def log_err(message: str) -> None:
    TERMINAL.error(message)


def log_verbose(message: str) -> None:
    TERMINAL.verbose(message)


def die(message: str, exit_code: int = 1) -> None:
    log_err(message)
    raise SystemExit(exit_code)


def parse_positive_int(value: str, name: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must be an integer") from exc
    if number <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")
    return number


def parse_positive_float(value: str, name: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must be a number") from exc
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")
    return number


# ---------------------------------------------------------------------------
# Command execution
# ---------------------------------------------------------------------------

def run_command(
    argv: Sequence[str],
    *,
    cwd: Optional[Path] = None,
    env: Optional[Dict[str, str]] = None,
    dry_run: bool = False,
    timeout_seconds: Optional[float] = None,
    grace_seconds: float = DEFAULT_COMMAND_GRACE_SECONDS,
    progress_label: Optional[str] = None,
) -> Dict[str, Any]:
    return COMMAND_RUNNER.run(
        argv,
        cwd=cwd,
        env=env,
        dry_run=dry_run,
        timeout_seconds=timeout_seconds,
        grace_seconds=grace_seconds,
        progress_label=progress_label,
    )


def command_with_sudo(
    argv: Sequence[str],
    use_sudo: bool,
    sudo_cmd: str,
    sudo_non_interactive: bool = True,
) -> List[str]:
    return SudoConfig(
        use_sudo=use_sudo,
        sudo_cmd=sudo_cmd,
        non_interactive=sudo_non_interactive,
    ).apply(argv)


def sysctl_value(name: str) -> str:
    return simple_command_value(["sysctl", "-n", name])


def check_tool(name: str, *, required: bool = True) -> Optional[str]:
    try:
        return common_check_tool(name, required=required)
    except FileNotFoundError:
        die(f"required tool not found in PATH: {name}")


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
    loaded = run_command(
        ["kldstat", "-n", "hwpmc"],
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if loaded["returncode"] == 0:
        status["was_loaded"] = True
        return status

    status["load_attempted"] = True
    load = run_command(
        command_with_sudo(
            ["kldload", "hwpmc"],
            args.use_sudo, args.sudo_cmd, args.sudo_non_interactive,
        ),
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if load["returncode"] == 0:
        status["loaded_by_collector"] = True
    else:
        status["error"] = load.get("stderr", "kldload hwpmc failed").strip()
    return status


def collect_platform_info(args: argparse.Namespace) -> Dict[str, Any]:
    cpuid = sysctl_value("kern.hwpmc.cpuid")
    parsed_cpuid = parse_hwpmc_cpuid(cpuid) if cpuid else parse_hwpmc_cpuid("")
    return {
        "collected_at": utc_now(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "uname": " ".join(platform.uname()),
        "hw_model": sysctl_value("hw.model"),
        "hw_ncpu": sysctl_value("hw.ncpu"),
        "hw_cpu_vendor": sysctl_value("hw.cpu_vendor"),
        "kern_hwpmc_cpuid": cpuid,
        "parsed_cpuid": parsed_cpuid,
        "pmcstat_path": shutil.which("pmcstat"),
        "cpuset_path": shutil.which("cpuset"),
        "timeout_path": shutil.which("timeout"),
    }


def validate_platform(info: Dict[str, Any], args: argparse.Namespace) -> None:
    if args.dry_run:
        return
    if info["system"] != "FreeBSD" and not args.allow_non_freebsd:
        die("this collector must run on FreeBSD")
    cpuid = info["parsed_cpuid"]
    if cpuid.get("vendor") != "AuthenticAMD" and not args.allow_non_amd:
        die("AMD core PMC collection requires AuthenticAMD CPU and kern.hwpmc.cpuid")


def collect_event_list(args: argparse.Namespace) -> List[str]:
    if args.dry_run:
        return []
    result = run_command(
        command_with_sudo(
            ["pmcstat", "-L"],
            args.use_sudo, args.sudo_cmd, args.sudo_non_interactive,
        ),
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if result.get("returncode") != 0:
        return []
    return [line.split()[0] for line in result.get("stdout", "").splitlines() if line.split()]


def check_event_availability(
    events: Iterable[str],
    required: Iterable[str],
) -> Dict[str, bool]:
    available = set(events)
    return {event: event in available for event in required}


# ---------------------------------------------------------------------------
# Output parsing
# ---------------------------------------------------------------------------

def parse_pmcstat_output(
    text: str,
    event_a: str,
    event_b: str,
    min_cycle_count: int = DEFAULT_MIN_CYCLE_COUNT,
) -> Dict[str, Any]:
    headers = [line for line in text.splitlines() if line.startswith("#")]
    rows: List[Tuple[int, int]] = []

    for line in text.splitlines():
        if line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[0].isdigit() and fields[1].isdigit():
            rows.append((int(fields[0]), int(fields[1])))

    header = next(
        (h for h in headers if event_a in h and event_b in h),
        None,
    )

    def count_event_in_header(h: Optional[str], ev: str) -> int:
        if h is None:
            return 0
        n = 0
        for token in h.split():
            if token == ev or token.endswith("/" + ev):
                n += 1
        return n

    event_a_count = count_event_in_header(header, event_a)
    event_b_count = count_event_in_header(header, event_b)
    header_valid = event_a_count >= 2 if event_a == event_b else (
        event_a_count >= 1 and event_b_count >= 1
    )

    parsed: Dict[str, Any] = {
        "headers": headers,
        "matching_header": header,
        "event_a_count": event_a_count,
        "event_b_count": event_b_count,
        "header_valid": header_valid,
        "rows": rows,
        "row_count": len(rows),
        "positive_a": sum(1 for a, _ in rows if a > 0),
        "positive_b": sum(1 for _, b in rows if b > 0),
        "positive_pairs": sum(1 for a, b in rows if a > 0 and b > 0),
        "event_a": event_a,
        "event_b": event_b,
        "last_a": None,
        "last_b": None,
        "total_a": None,
        "total_b": None,
        "measurement_valid": False,
        "low_count_warning": False,
    }

    if rows:
        last_a, last_b = rows[-1]
        total_a = sum(a for a, _ in rows)
        total_b = sum(b for _, b in rows)
        parsed["last_a"] = last_a
        parsed["last_b"] = last_b
        parsed["total_a"] = total_a
        parsed["total_b"] = total_b
        minimum = min_cycle_count if event_a == event_b == PMSTAT_DUPLICATE_EVENT else 1
        parsed["measurement_valid"] = bool(
            header_valid and last_a >= minimum and last_b >= minimum
        )
        # Warn if counts are below the CPU-bound expectation
        parsed["low_count_warning"] = bool(last_a < DEFAULT_MIN_CYCLE_COUNT)

        if event_a == event_b and max(last_a, last_b) > 0:
            delta = abs(last_a - last_b)
            maximum = max(last_a, last_b)
            total_delta = abs(total_a - total_b)
            total_maximum = max(total_a, total_b)
            parsed["delta"] = delta
            parsed["max"] = maximum
            parsed["permille"] = (delta * 1000.0) / maximum
            parsed["total_delta"] = total_delta
            parsed["total_max"] = total_maximum
            parsed["total_permille"] = (
                (total_delta * 1000.0) / total_maximum if total_maximum > 0 else None
            )

    return parsed


# ---------------------------------------------------------------------------
# Summary helpers
# ---------------------------------------------------------------------------

def summarize_skew(records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    permilles = [
        float(r["permille"])
        for r in records
        if r.get("permille") is not None
    ]
    accepted = [r for r in records if r.get("accepted") is True]
    rejected = [r for r in records if r.get("accepted") is False]
    verdicts = len(accepted) + len(rejected)
    low_count_warnings = sum(1 for r in records if r.get("low_count_warning"))

    mean_p = statistics.fmean(permilles) if permilles else None
    stdev_p = statistics.pstdev(permilles) if len(permilles) > 1 else 0.0
    p50 = percentile(permilles, 0.50)
    p90 = percentile(permilles, 0.90)
    p95 = percentile(permilles, 0.95)

    return {
        "samples": len(records),
        "samples_with_permille": len(permilles),
        "accepted": len(accepted),
        "rejected": len(rejected),
        "low_count_warnings": low_count_warnings,
        "acceptance_rate": (len(accepted) / verdicts) if verdicts else None,
        "min_permille": min(permilles) if permilles else None,
        "max_permille": max(permilles) if permilles else None,
        "mean_permille": mean_p,
        "stdev_permille": stdev_p,
        "p50_permille": p50,
        "p90_permille": p90,
        "p95_permille": p95,
        "mean_percent": permille_to_percent(mean_p),
        "p90_percent": permille_to_percent(p90),
        "p95_percent": permille_to_percent(p95),
        "max_percent": permille_to_percent(max(permilles) if permilles else None),
    }


def build_summary(state: Dict[str, Any]) -> Dict[str, Any]:
    records = state.get("records", [])
    return {
        "updated_at": utc_now(),
        "schema_version": SCHEMA_VERSION,
        "producer": PRODUCER,
        "workload_cmd": state.get("configuration", {}).get("workload_cmd", []),
        "iterations_completed": state.get("iterations_completed", 0),
        "records": len(records),
        "skew": summarize_skew(records),
    }


# ---------------------------------------------------------------------------
# JSON / CSV emission
# ---------------------------------------------------------------------------

CSV_COLUMNS = [
    "run_id",
    "iteration",
    "started_at",
    "ended_at",
    "elapsed_seconds",
    "returncode",
    "status",
    "command_outcome",
    "measurement_valid",
    "low_count_warning",
    "tolerance_pass",
    "accepted",
    "tolerance_permille",
    "event_a",
    "event_b",
    "row_count",
    "positive_a",
    "positive_b",
    "last_a",
    "last_b",
    "total_a",
    "total_b",
    "delta",
    "max",
    "permille",
    "total_permille",
    "output_file",
    "raw_sha256",
    "command_text",
    "workload_cmd",
]


def record_to_csv_row(record: Dict[str, Any]) -> Dict[str, Any]:
    row = {col: record.get(col, "") for col in CSV_COLUMNS}
    if isinstance(row.get("workload_cmd"), list):
        row["workload_cmd"] = shlex.join(row["workload_cmd"])
    return row


def write_csv_records(path: Path, records: Sequence[Dict[str, Any]]) -> None:
    tmp = unique_tmp_path(path)
    try:
        with tmp.open("w", newline="", encoding="utf-8") as fp:
            writer = csv.DictWriter(fp, fieldnames=CSV_COLUMNS)
            writer.writeheader()
            for record in records:
                writer.writerow(record_to_csv_row(record))
            fp.flush()
            os.fsync(fp.fileno())
        tmp.replace(path)
        fsync_directory(path.parent)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


def write_state(state: Dict[str, Any], json_path: Path, csv_path: Path) -> None:
    state["summary"] = build_summary(state)
    atomic_write_text(json_path, json.dumps(state, indent=2, sort_keys=True) + "\n")
    write_csv_records(csv_path, state.get("records", []))


class CheckpointWriter:
    def __init__(self, json_path: Path, csv_path: Path, threaded: bool) -> None:
        self.json_path = json_path
        self.csv_path = csv_path
        self.threaded = threaded
        self._queue: "queue.Queue[Optional[Dict[str, Any]]]" = queue.Queue(maxsize=2)
        self._thread: Optional[threading.Thread] = None
        self._error: Optional[BaseException] = None

    def start(self) -> None:
        if not self.threaded:
            return
        self._thread = threading.Thread(
            target=self._worker,
            name="pmc-skew-v2-writer",
            daemon=True,
        )
        self._thread.start()

    def write(self, state: Dict[str, Any]) -> None:
        self.raise_if_failed()
        snapshot = copy.deepcopy(state)
        if not self.threaded:
            write_state(snapshot, self.json_path, self.csv_path)
            return
        while True:
            try:
                self._queue.put(snapshot, timeout=0.25)
                return
            except queue.Full:
                self.raise_if_failed()

    def close(self) -> None:
        if not self.threaded:
            self.raise_if_failed()
            return
        while True:
            try:
                self._queue.put(None, timeout=0.25)
                break
            except queue.Full:
                self.raise_if_failed()
        if self._thread is not None:
            self._thread.join()
        self.raise_if_failed()

    def raise_if_failed(self) -> None:
        if self._error is not None:
            raise RuntimeError("checkpoint writer failed") from self._error

    def _worker(self) -> None:
        latest: Optional[Dict[str, Any]] = None
        try:
            while True:
                item = self._queue.get()
                if item is None:
                    if latest is not None:
                        write_state(latest, self.json_path, self.csv_path)
                    return
                latest = item
                while True:
                    try:
                        extra = self._queue.get_nowait()
                    except queue.Empty:
                        break
                    if extra is None:
                        write_state(latest, self.json_path, self.csv_path)
                        return
                    latest = extra
                write_state(latest, self.json_path, self.csv_path)
                latest = None
        except BaseException as exc:
            self._error = exc


# ---------------------------------------------------------------------------
# Terminal output
# ---------------------------------------------------------------------------

def color_status(status: str) -> str:
    if status == "PASS":
        return colorize(status, "green")
    if status == "FAIL":
        return colorize(status, "red")
    if status in ("SKIP", "STOP", "WARN"):
        return colorize(status, "yellow")
    return colorize(status, "cyan")


def print_record_line(record: Dict[str, Any], tolerance: int) -> None:
    permille = record.get("permille")
    accepted = record.get("accepted")
    status = (
        "PASS" if accepted is True
        else "FAIL" if accepted is False
        else "INFO"
    )
    last_a = record.get("last_a")
    last_b = record.get("last_b")
    warn = " [low-count-warn]" if record.get("low_count_warning") else ""

    if permille is None:
        log_info(
            f"iteration {record.get('iteration')}: {color_status(status)}"
            f" ({record.get('status', '')}){warn}"
        )
        return

    log_info(
        f"iteration {record.get('iteration')}: {color_status(status)}"
        f" permille={float(permille):.3f} tolerance={tolerance}"
        f" last={last_a}/{last_b}{warn}"
    )


def print_final_summary(state: Dict[str, Any], json_path: Path, csv_path: Path) -> None:
    summary = state.get("summary") or build_summary(state)
    skew = summary.get("skew", {})
    configuration = state.get("configuration", {})
    tolerance = int(configuration.get("tolerance_permille", 0) or 0)
    workload_cmd = shlex.join(configuration.get("workload_cmd", []))

    log_info("=== v2 skew summary ===")
    if state.get("shutdown_reason"):
        log_info(f"shutdown: {state['shutdown_reason']}")
    log_info(f"workload: {workload_cmd}")
    log_info(f"tolerance: {format_skew(float(tolerance))}")

    samples = skew.get("samples", 0)
    if samples == 0:
        log_info("no samples collected")
    else:
        rate = skew.get("acceptance_rate")
        rate_s = "n/a" if rate is None else f"{rate * 100.0:.2f}%"
        log_info(
            f"samples={samples} measurements={skew.get('samples_with_permille')}"
            f" accepted={skew.get('accepted')} rejected={skew.get('rejected')}"
            f" acceptance={rate_s}"
        )
        log_info(
            f"mean={format_skew(skew.get('mean_permille'))}"
            f" p50={format_skew(skew.get('p50_permille'))}"
            f" p90={format_skew(skew.get('p90_permille'))}"
            f" p95={format_skew(skew.get('p95_permille'))}"
            f" max={format_skew(skew.get('max_permille'))}"
        )
        if skew.get("low_count_warnings", 0):
            log_warn(
                f"low_count_warnings={skew['low_count_warnings']}: "
                "some runs had cycle counts below the CPU-bound threshold; "
                "verify that cpuset and dd executed correctly"
            )

    log_info(f"JSON: {json_path}")
    log_info(f"CSV:  {csv_path}")


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def run_probe(
    args: argparse.Namespace,
    *,
    run_id: str,
    iteration: int,
    raw_dir: Path,
) -> Dict[str, Any]:
    output_file = raw_dir / f"{iteration:05d}-duplicate-cycles-v2.out"
    tmp_output = output_file if args.dry_run else unique_tmp_path(output_file, "out")

    # Build the pmcstat command:
    #   pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -o <file>
    #       -- <workload_cmd>
    command = [
        "pmcstat", "-C", "-q",
        "-p", PMSTAT_DUPLICATE_EVENT,
        "-p", PMSTAT_DUPLICATE_EVENT,
        "-o", str(tmp_output),
        "--",
    ] + list(args.workload_cmd)

    timeout_s = bounded_timeout(
        args.deadline,
        args.command_timeout_seconds,
    )

    result = run_command(
        command_with_sudo(
            command,
            args.use_sudo, args.sudo_cmd, args.sudo_non_interactive,
        ),
        dry_run=args.dry_run,
        timeout_seconds=timeout_s,
        grace_seconds=args.command_grace_seconds,
        progress_label=f"v2 probe iteration {iteration}",
    )

    if tmp_output.exists() and result.get("returncode") == 0:
        tmp_output.replace(output_file)
        fsync_directory(output_file.parent)
    elif not args.dry_run:
        for stale in (tmp_output, output_file):
            try:
                stale.unlink()
            except OSError:
                pass

    output_text = "" if args.dry_run else read_text_file(output_file)
    parsed = parse_pmcstat_output(
        output_text,
        PMSTAT_DUPLICATE_EVENT,
        PMSTAT_DUPLICATE_EVENT,
        args.min_cycle_count,
    )

    rc = result.get("returncode")
    terminated_by_signal = result.get("terminated_by_signal")
    interrupted = terminated_by_signal in (signal.SIGINT, signal.SIGTERM)
    command_ok = rc == 0 and result.get("outcome") in ("ok", "dry_run")
    measurement_valid = bool(parsed.get("measurement_valid"))
    permille = parsed.get("permille")

    tolerance_pass: Optional[bool] = None
    accepted: Optional[bool]
    status: str

    if interrupted or (STOP_REQUESTED and not command_ok and not result.get("dry_run")):
        accepted = None
        status = "interrupted"
    elif result.get("dry_run"):
        accepted = None
        status = "dry_run"
    else:
        valid_for_tolerance = measurement_valid
        tolerance_pass = bool(
            command_ok
            and valid_for_tolerance
            and permille is not None
            and float(permille) <= args.tolerance_permille
        )
        accepted = tolerance_pass
        if not command_ok:
            status = str(result.get("outcome", "command_error"))
        elif permille is None or not valid_for_tolerance:
            status = "invalid_output"
        else:
            status = "pass" if tolerance_pass else "fail_tolerance"

    record: Dict[str, Any] = {
        "run_id": run_id,
        "iteration": iteration,
        "started_at": result.get("started_at"),
        "ended_at": result.get("ended_at"),
        "elapsed_seconds": result.get("elapsed_seconds"),
        "returncode": rc,
        "terminated_by_signal": terminated_by_signal,
        "status": status,
        "command_outcome": result.get("outcome"),
        "measurement_valid": measurement_valid,
        "low_count_warning": bool(parsed.get("low_count_warning")),
        "tolerance_pass": tolerance_pass,
        "accepted": accepted,
        "tolerance_permille": args.tolerance_permille,
        "event_a": PMSTAT_DUPLICATE_EVENT,
        "event_b": PMSTAT_DUPLICATE_EVENT,
        "row_count": parsed.get("row_count"),
        "positive_a": parsed.get("positive_a"),
        "positive_b": parsed.get("positive_b"),
        "positive_pairs": parsed.get("positive_pairs"),
        "last_a": parsed.get("last_a"),
        "last_b": parsed.get("last_b"),
        "total_a": parsed.get("total_a"),
        "total_b": parsed.get("total_b"),
        "delta": parsed.get("delta"),
        "max": parsed.get("max"),
        "permille": permille,
        "total_permille": parsed.get("total_permille"),
        "output_file": str(output_file) if not args.dry_run else None,
        "raw_sha256": sha256_file(output_file) if not args.dry_run and output_file.exists() else None,
        "command_text": result.get("command_text"),
        "workload_cmd": list(args.workload_cmd),
        "stdout_tail": tail_text(result.get("stdout", "")),
        "stderr_tail": tail_text(result.get("stderr", "")),
        "parsed": parsed,
        "command": result,
    }
    return record


def enough_time_remains(args: argparse.Namespace, needed: float) -> bool:
    remaining = remaining_seconds(args.deadline)
    if remaining is None:
        return True
    return remaining > max(1.0, needed)


# ---------------------------------------------------------------------------
# Signal handling
# ---------------------------------------------------------------------------

def signal_handler(signum: int, _frame: Any) -> None:
    global SHUTDOWN_REASON, STOP_REQUESTED
    STOP_REQUESTED = True
    SHUTDOWN_REASON = f"signal {signum}"
    COMMAND_RUNNER.terminate_active(signal.SIGTERM)
    log_warn(f"received signal {signum}; finishing current probe then stopping")


def shutdown_exit_code() -> int:
    if SHUTDOWN_REASON is None:
        return 0
    match = re.fullmatch(r"signal (\d+)", SHUTDOWN_REASON)
    if match is None:
        return 0
    return 128 + int(match.group(1))


# ---------------------------------------------------------------------------
# Argument parsing and main loop
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    default_outdir = Path.cwd() / "pmu-skew-data-v2"
    workload_default = shlex.join(DEFAULT_WORKLOAD_CMD)

    parser = argparse.ArgumentParser(
        description=(
            "v2 collector: AMD pmcstat grouping skew with CPU-bound workload + CPU pinning.\n"
            "Fixes v1 flakiness by replacing idle 'sleep N' with 'cpuset -l 0 timeout 5 dd'."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_v2_collect.py --minutes 120\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_v2_collect.py --once\n"
            "  python3 py-scripts/pmc_grouping_skew_v2_collect.py --dry-run --once\n"
        ),
    )

    parser.add_argument("--minutes", type=lambda v: parse_positive_float(v, "minutes"),
        default=DEFAULT_MINUTES,
        help=f"collection window in minutes (default: {DEFAULT_MINUTES})")
    parser.add_argument("--iterations", type=lambda v: parse_positive_int(v, "iterations"),
        default=None, help="optional maximum number of iterations")
    parser.add_argument("--once", action="store_true",
        help="run exactly one iteration")

    parser.add_argument("--workload-cmd", nargs="+", default=DEFAULT_WORKLOAD_CMD,
        help=f"workload command monitored by pmcstat (default: {workload_default})")
    parser.add_argument("--tolerance-permille",
        type=lambda v: parse_positive_int(v, "tolerance-permille"),
        default=DEFAULT_TOLERANCE_PERMILLE,
        help=(
            f"duplicate-cycle acceptance tolerance in permille "
            f"(default: {DEFAULT_TOLERANCE_PERMILLE}; "
            f"v1 empirical max ~123‰ — v2 expected <20‰)"
        ))
    parser.add_argument("--min-cycle-count",
        type=lambda v: parse_positive_int(v, "min-cycle-count"),
        default=DEFAULT_MIN_CYCLE_COUNT,
        help=(
            f"minimum cycle count for a valid measurement "
            f"(default: {DEFAULT_MIN_CYCLE_COUNT:,}; "
            f"below this triggers a low-count warning)"
        ))

    parser.add_argument("--outdir", type=Path, default=default_outdir,
        help="output directory for JSON, CSV, and raw files (default: ./pmu-skew-data-v2)")

    parser.add_argument("--sudo-cmd", default="sudo")
    parser.add_argument("--sudo-interactive", dest="sudo_non_interactive",
        action="store_false")
    parser.add_argument("--no-sudo", dest="use_sudo", action="store_false")
    parser.set_defaults(use_sudo=True, sudo_non_interactive=True)

    parser.add_argument("--command-timeout-seconds",
        type=lambda v: parse_positive_int(v, "command-timeout-seconds"),
        default=DEFAULT_COMMAND_TIMEOUT_SECONDS,
        help=f"per-probe timeout in seconds (default: {DEFAULT_COMMAND_TIMEOUT_SECONDS})")
    parser.add_argument("--preflight-timeout",
        type=lambda v: parse_positive_int(v, "preflight-timeout"),
        default=30)
    parser.add_argument("--command-grace-seconds",
        type=lambda v: parse_positive_int(v, "command-grace-seconds"),
        default=DEFAULT_COMMAND_GRACE_SECONDS)
    parser.add_argument("--no-threaded-writer", dest="threaded_writer",
        action="store_false")
    parser.set_defaults(threaded_writer=True)

    parser.add_argument("--color", choices=("auto", "always", "never"), default="auto")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-hwpmc-load", action="store_true")
    parser.add_argument("--allow-non-freebsd", action="store_true")
    parser.add_argument("--allow-non-amd", action="store_true")

    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    configure_terminal(args)
    if args.once:
        args.iterations = 1
    if args.dry_run and args.iterations is None:
        args.iterations = 1
    args.outdir = args.outdir.expanduser().resolve()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    if not args.dry_run:
        check_tool("pmcstat")
        if args.use_sudo and not is_root():
            check_tool(args.sudo_cmd)
            if args.sudo_non_interactive:
                sudo_check = run_command(
                    [args.sudo_cmd, "-n", "true"],
                    timeout_seconds=args.preflight_timeout,
                    grace_seconds=args.command_grace_seconds,
                )
                if sudo_check.get("returncode") != 0:
                    die("sudo -n true failed; run as root or use --sudo-interactive")

    args.outdir.mkdir(parents=True, exist_ok=True)
    raw_dir = args.outdir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.outdir / "pmc-grouping-skew-v2.json"
    csv_path = args.outdir / "pmc-grouping-skew-v2.csv"

    hwpmc_status = ensure_hwpmc_loaded(args)
    if hwpmc_status.get("error") and not args.dry_run and platform.system() == "FreeBSD":
        die(f"hwpmc unavailable: {hwpmc_status['error']}")

    platform_info = collect_platform_info(args)
    validate_platform(platform_info, args)

    cpuid = platform_info.get("parsed_cpuid", {})
    log_info(
        "target CPU: "
        f"{cpuid.get('generation')} family={cpuid.get('family_hex')} "
        f"model={cpuid.get('model_hex')} stepping={cpuid.get('stepping')}"
    )
    log_info(f"workload: {shlex.join(args.workload_cmd)}")
    log_info(f"tolerance: {args.tolerance_permille}‰")

    events = collect_event_list(args)
    availability = check_event_availability(events, [PMSTAT_DUPLICATE_EVENT])
    if not availability.get(PMSTAT_DUPLICATE_EVENT) and not args.dry_run:
        die(f"required event {PMSTAT_DUPLICATE_EVENT} not listed by pmcstat -L")
    args.event_availability = availability

    run_id = uuid.uuid4().hex
    state: Dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "producer": PRODUCER,
        "run_id": run_id,
        "started_at": utc_now(),
        "ended_at": None,
        "iterations_completed": 0,
        "configuration": {
            "minutes": args.minutes,
            "iterations": args.iterations,
            "workload_cmd": list(args.workload_cmd),
            "tolerance_permille": args.tolerance_permille,
            "min_cycle_count": args.min_cycle_count,
            "command_timeout_seconds": args.command_timeout_seconds,
            "use_sudo": args.use_sudo,
            "sudo_cmd": args.sudo_cmd,
            "dry_run": args.dry_run,
        },
        "platform": platform_info,
        "hwpmc": hwpmc_status,
        "event_availability": availability,
        "records": [],
        "summary": {},
    }

    writer = CheckpointWriter(json_path, csv_path, args.threaded_writer)
    writer.start()
    writer.write(state)

    deadline = time.monotonic() + args.minutes * 60.0
    args.deadline = deadline
    iteration = 0

    try:
        while not STOP_REQUESTED:
            if args.iterations is not None and iteration >= args.iterations:
                break
            if time.monotonic() >= deadline:
                break
            if not enough_time_remains(args, args.command_timeout_seconds):
                log_verbose("stopping: insufficient time for another probe")
                break

            iteration += 1
            log_info(f"starting iteration {iteration}")
            record = run_probe(args, run_id=run_id, iteration=iteration, raw_dir=raw_dir)
            state["records"].append(record)
            state["iterations_completed"] = iteration
            writer.write(state)

            if not args.quiet:
                print_record_line(record, args.tolerance_permille)

        state["ended_at"] = utc_now()
        if SHUTDOWN_REASON is not None:
            state["shutdown_reason"] = SHUTDOWN_REASON
        writer.write(state)
        writer.close()
        print_final_summary(state, json_path, csv_path)
        return shutdown_exit_code()

    except KeyboardInterrupt:
        state["ended_at"] = utc_now()
        state["shutdown_reason"] = "keyboard interrupt"
        writer.write(state)
        writer.close()
        print_final_summary(state, json_path, csv_path)
        return 130

    except BaseException:
        state["ended_at"] = utc_now()
        state["shutdown_reason"] = "exception"
        writer.write(state)
        writer.close()
        raise


if __name__ == "__main__":
    sys.exit(main())

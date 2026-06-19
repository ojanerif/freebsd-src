#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from freebsd_cache_hotspot import _fbcacheprobe as _fb
from freebsd_cache_hotspot.analyzer import summarize_cache_sources, summarize_pipeline
from freebsd_cache_hotspot.collector import FreeBsdProfiler, run_pmcstat_ibs
from freebsd_cache_hotspot.events import default_events
from freebsd_cache_hotspot.sweep import render_final_heatmap, run_parallel_sweep, sweep_to_csv
from freebsd_cache_hotspot.topology import Topology
from freebsd_cache_hotspot import report


def _split_events(text: str | None) -> list[str] | None:
    if not text:
        return None
    return [item.strip() for item in text.split(",") if item.strip()]


def _normalize_remainder(command: list[str]) -> list[str]:
    if command[:1] == ["--"]:
        return command[1:]
    return command


def _write_new_text(path: str, text: str) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    fd = os.open(os.fspath(Path(path)), flags, 0o644)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            fd = -1
            out.write(text)
    finally:
        if fd >= 0:
            os.close(fd)


def _add_cache_probe_args(parser: argparse.ArgumentParser, *, steps_default: int, repeat_default: int) -> None:
    parser.add_argument("--min-kb", type=int, default=1)
    parser.add_argument("--max-kb", type=int, default=256 * 1024)
    parser.add_argument("--steps", type=int, default=steps_default)
    parser.add_argument("--repeat", type=int, default=repeat_default)
    parser.add_argument("--no-superpage", action="store_false", dest="superpage")


def cmd_topo(args: argparse.Namespace) -> None:
    report.print_topology(Topology())


def cmd_probe(args: argparse.Namespace) -> None:
    if args.min_kb < 1 or args.max_kb < args.min_kb:
        raise SystemExit("--min-kb must be >= 1 and <= --max-kb")
    topo = Topology()
    report.print_topology(topo)
    rows = _fb.cache_latency_probe(
        min_kb=args.min_kb,
        max_kb=args.max_kb,
        steps=args.steps,
        repeat=args.repeat,
        cpu=args.cpu,
        superpage=args.superpage,
    )
    report.print_latency_curve(rows)
    if args.bandwidth:
        bw = _fb.cache_bw_probe(
            min_kb=args.min_kb,
            max_kb=args.max_kb,
            steps=args.steps,
            repeat=args.bw_repeat,
            cpu=args.cpu,
            superpage=args.superpage,
        )
        report.print_bandwidth_curve(bw)


def cmd_count(args: argparse.Namespace) -> None:
    if not args.command:
        raise SystemExit("count requires a command after --")
    if args.events and args.topdown:
        raise SystemExit("--events and --topdown are mutually exclusive")
    topo = Topology()
    profiler = FreeBsdProfiler(topo)
    events = _split_events(args.events)
    if events is None:
        events = default_events(topo.cpu.family, topo.cpu.model, topdown=args.topdown)
    result = profiler.count_command(
        args.command,
        events,
        mode="SC" if args.system else "TC",
        cpu=args.cpu,
        topdown=args.topdown,
    )
    report.print_topology(topo)
    report.print_counts(result)
    report.print_cache_summary(summarize_cache_sources(result.counts))
    report.print_pipeline_summary(summarize_pipeline(result.counts, topo.cpu.pipeline_width))
    if args.json:
        _write_new_text(args.json, report.counts_json(result))


def cmd_sweep(args: argparse.Namespace) -> None:
    # --top-hotspots is CLI-only; min/max/steps/repeat are validated by the
    # library. Bridge ValueError -> SystemExit so direct cmd_sweep callers
    # (e.g. tests) see the same shape as main().
    if args.top_hotspots < 1:
        raise SystemExit("--top-hotspots must be >= 1")
    topo = Topology()
    report.print_topology(topo)
    sys.stdout.write("\n")
    try:
        result = run_parallel_sweep(
            topo,
            min_kb=args.min_kb,
            max_kb=args.max_kb,
            steps=args.steps,
            repeat=args.repeat,
            superpage=args.superpage,
            color=not args.no_color,
            scale_mode=args.scale,
        )
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    render_final_heatmap(
        result, color=not args.no_color, scale_mode=args.scale, top=args.top_hotspots,
    )
    if args.csv:
        _write_new_text(args.csv, sweep_to_csv(result))
        sys.stdout.write(f"\nCSV: {args.csv}\n")


def cmd_ibs(args: argparse.Namespace) -> None:
    if not args.command:
        raise SystemExit("ibs requires a command after --")
    topo = Topology()
    if not topo.pmc.ibs_op:
        raise SystemExit("IBS Op sampling is not advertised by CPUID 0x8000001B EAX[2]")
    if args.ldlat and not topo.pmc.ibs_ldlat:
        raise SystemExit(
            "IBS load-latency filter (--ldlat) requires CPUID 0x8000001B EAX[12]; "
            "available on Zen 5 only"
        )
    result = run_pmcstat_ibs(
        args.command,
        period=args.period,
        l3miss=args.l3miss,
        ldlat=args.ldlat,
        opcount=args.opcount,
        top=args.top,
        outdir=args.outdir,
    )
    report.print_ibs_result(result)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="freebsd-cache-hotspot",
        description="AMD Zen cache-hotspot localization for FreeBSD hwpmc",
    )
    sub = parser.add_subparsers(dest="subcmd", required=True)

    topo = sub.add_parser("topo", help="show CPUID, cache, IBS, and hwpmc capabilities")
    topo.set_defaults(func=cmd_topo)

    probe = sub.add_parser("probe", help="run native C cache latency/bandwidth probes")
    _add_cache_probe_args(probe, steps_default=50, repeat_default=5)
    probe.add_argument("--bw-repeat", type=int, default=3)
    probe.add_argument("--cpu", type=int, default=0, help="CPU to pin the measuring thread to")
    probe.add_argument("--bandwidth", action="store_true")
    probe.set_defaults(func=cmd_probe, superpage=True)

    count = sub.add_parser("count", help="count AMD core PMCs around a command")
    count.add_argument("--events", help="comma-separated event keys or FreeBSD PMU event names")
    count.add_argument("--topdown", action="store_true", help="use Zen 4 dispatch-slot approximation events")
    count.add_argument("--system", action="store_true", help="use PMC_MODE_SC on --cpu instead of process PMC_MODE_TC")
    count.add_argument("--cpu", type=int, default=0, help="CPU for system-scope PMCs")
    count.add_argument("--json", help="write count payload as JSON")
    count.add_argument("command", nargs=argparse.REMAINDER)
    count.set_defaults(func=cmd_count)

    sweep = sub.add_parser("sweep", help="parallel per-CCX cache latency sweep with live heatmap")
    _add_cache_probe_args(sweep, steps_default=40, repeat_default=3)
    sweep.add_argument("--no-color", action="store_true")
    sweep.add_argument(
        "--scale", choices=("robust", "linear"), default="robust",
        help="heat scale: robust clips to p5/p95, linear uses min/max",
    )
    sweep.add_argument(
        "--top-hotspots", type=int, default=8,
        help="number of hotspot rows in terminal reports",
    )
    sweep.add_argument("--csv", help="write CCX/bucket samples as CSV")
    sweep.set_defaults(func=cmd_sweep, superpage=True)

    ibs = sub.add_parser("ibs", help="run FreeBSD pmcstat IBS Op sampling for a command")
    ibs.add_argument("--period", type=int, default=65536, help="IBS Op sample period; FreeBSD minimum is 65536")
    ibs.add_argument("--l3miss", action="store_true", help="add the FreeBSD ibs-op,l3miss qualifier")
    ibs.add_argument("--ldlat", type=int, default=0, help="load-latency filter, 128..2048 cycles; implies l3miss")
    ibs.add_argument("--opcount", action="store_true", help="add the FreeBSD ibs-op,opcount qualifier")
    ibs.add_argument("--top", type=int, default=30)
    ibs.add_argument("--outdir", help="directory for ibs-op.pmc; defaults to a temporary directory")
    ibs.add_argument("command", nargs=argparse.REMAINDER)
    ibs.set_defaults(func=cmd_ibs)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "command", None):
        args.command = _normalize_remainder(args.command)
    try:
        args.func(args)
    except (OSError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

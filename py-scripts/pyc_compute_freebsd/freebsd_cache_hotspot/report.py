#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

import json
import sys
from dataclasses import asdict
from typing import Sequence, TextIO

from freebsd_cache_hotspot.analyzer import CacheSourceSummary, CountResult, PipelineSummary
from freebsd_cache_hotspot.collector import IbsTextResult, ProfileResult
from freebsd_cache_hotspot.topology import Topology, fmt_kb


def print_topology(topo: Topology, out: TextIO = sys.stdout) -> None:
    out.write(f"CPU:     {topo.cpu.brand}\n")
    out.write(
        f"Silicon: Family {topo.cpu.family:#x} Model {topo.cpu.model:#x} "
        f"Step {topo.cpu.stepping} ({topo.cpu.generation})\n"
    )
    if topo.cpu.pipeline_width:
        out.write(f"Width:   dispatch {topo.cpu.pipeline_width}-wide\n")
    else:
        out.write("Width:   dispatch width unknown for this generation\n")
    out.write(f"Cores:   {topo.online_cpus} logical, {topo.numa_domains} NUMA domain(s)\n")
    out.write(f"TSC:     {topo.tsc_khz / 1000.0:.1f} MHz\n")
    l1d = topo.cache_size_kb(1, "Data")
    l2 = topo.cache_size_kb(2)
    l3 = topo.cache_size_kb(3)
    out.write(f"Cache:   L1d {fmt_kb(l1d or 32)}  L2 {fmt_kb(l2 or 1024)}  L3 {fmt_kb(l3)}\n")
    out.write(
        f"CPUID:   PerfMonV2={'yes' if topo.pmc.perfmon_v2 else 'no'}  "
        f"core PMCs={topo.pmc.core_counters}  DF PMCs={topo.pmc.df_counters}\n"
    )
    out.write(
        f"IBS:     fetch={'yes' if topo.pmc.ibs_fetch else 'no'}  "
        f"op={'yes' if topo.pmc.ibs_op else 'no'}  "
        f"Zen4Ext={'yes' if topo.pmc.ibs_zen4_ext else 'no'}  "
        f"LdLat={'yes' if topo.pmc.ibs_ldlat else 'no'}\n"
    )
    out.write(f"hwpmc:   {'available' if topo.pmc.hwpmc_available else 'unavailable'}")
    if topo.pmc.hwpmc_info.get("error"):
        out.write(f" ({topo.pmc.hwpmc_info['error']})")
    out.write("\n")


def print_latency_curve(rows: Sequence[tuple[int, float]], out: TextIO = sys.stdout) -> None:
    out.write("\nLatency probe (dependent load chain)\n")
    out.write("size_kb,ns_per_load\n")
    for kb, ns in rows:
        out.write(f"{kb},{ns:.3f}\n")


def print_bandwidth_curve(rows: Sequence[tuple[int, float]], out: TextIO = sys.stdout) -> None:
    out.write("\nBandwidth probe (sequential read)\n")
    out.write("size_kb,gbps\n")
    for kb, gbps in rows:
        out.write(f"{kb},{gbps:.3f}\n")


def print_counts(result: ProfileResult, out: TextIO = sys.stdout) -> None:
    out.write(f"\nCounter run: mode={result.mode} cpu={result.cpu} duration={result.duration_ns / 1e9:.6f}s\n")
    out.write("event,spec,value,description\n")
    for row in result.counts:
        out.write(f"{row.name},{row.freebsd_spec},{row.value},{row.description}\n")
    if result.skipped_events:
        out.write("Skipped/notes:\n")
        for item in result.skipped_events:
            out.write(f"  - {item}\n")


def print_cache_summary(summary: CacheSourceSummary, out: TextIO = sys.stdout) -> None:
    out.write("\nCache-source summary\n")
    out.write(f"  L3/same-CCX fills: {summary.l3_or_same_ccx:,}\n")
    out.write(f"  near DRAM fills:   {summary.near_dram:,}\n")
    out.write(f"  far DRAM fills:    {summary.far_dram:,}\n")
    out.write(f"  remote DRAM frac:  {summary.remote_dram_fraction * 100.0:.2f}%\n")
    for note in summary.notes:
        out.write(f"  note: {note}\n")


def print_pipeline_summary(summary: PipelineSummary | None, out: TextIO = sys.stdout) -> None:
    if summary is None:
        return
    out.write("\nAMD TMA approximation\n")
    out.write(f"  IPC:              {summary.ipc:.3f}\n")
    out.write(f"  frontend bound:   {summary.frontend_bound * 100.0:.2f}%\n")
    out.write(f"  backend bound:    {summary.backend_bound * 100.0:.2f}%\n")
    out.write(f"  SMT contention:   {summary.smt_contention * 100.0:.2f}%\n")
    out.write(f"  retiring:         {summary.retiring * 100.0:.2f}%\n")
    out.write(f"  bad speculation:  {summary.bad_speculation * 100.0:.2f}%\n")
    for note in summary.notes:
        out.write(f"  note: {note}\n")


def print_ibs_result(result: IbsTextResult, out: TextIO = sys.stdout) -> None:
    out.write(f"IBS event: {result.event_spec} period={result.period}\n")
    out.write(f"pmclog:    {result.log_path}\n")
    out.write(
        f"Decoded:   ibs-op lines={result.op_samples} load={result.load_samples} "
        f"miss={result.miss_samples} latency={result.latency_lines}\n\n"
    )
    out.write(result.decoded_output)


def counts_json(result: ProfileResult) -> str:
    payload = {
        "command": result.command,
        "mode": result.mode,
        "cpu": result.cpu,
        "pid": result.pid,
        "duration_ns": result.duration_ns,
        "counts": [asdict(row) for row in result.counts],
        "skipped_events": result.skipped_events,
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"

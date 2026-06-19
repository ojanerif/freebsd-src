#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

"""Counter-derived AMD Zen cache and pipeline summaries."""

from dataclasses import dataclass, field
from typing import Sequence


@dataclass(frozen=True, slots=True)
class CountResult:
    name: str
    freebsd_spec: str
    value: int
    description: str = ""


@dataclass(slots=True)
class CacheSourceSummary:
    l3_or_same_ccx: int = 0
    near_dram: int = 0
    far_dram: int = 0
    l2_miss: int = 0
    notes: list[str] = field(default_factory=list)

    @property
    def dram_total(self) -> int:
        return self.near_dram + self.far_dram

    @property
    def remote_dram_fraction(self) -> float:
        if self.dram_total == 0:
            return 0.0
        return self.far_dram / self.dram_total


@dataclass(slots=True)
class PipelineSummary:
    ipc: float = 0.0
    frontend_bound: float = 0.0
    backend_bound: float = 0.0
    smt_contention: float = 0.0
    retiring: float = 0.0
    bad_speculation: float = 0.0
    notes: list[str] = field(default_factory=list)


def _counts_by_name(counts: Sequence[CountResult]) -> dict[str, int]:
    return {row.name: row.value for row in counts}


def summarize_cache_sources(counts: Sequence[CountResult]) -> CacheSourceSummary:
    values = _counts_by_name(counts)
    summary = CacheSourceSummary(
        l3_or_same_ccx=values.get("dc_fill_l3", 0),
        near_dram=values.get("dc_fill_near_dram", 0),
        far_dram=values.get("dc_fill_far_dram", 0),
        l2_miss=values.get("l2_miss", 0),
    )
    if summary.remote_dram_fraction > 0.10:
        summary.notes.append("far DRAM exceeds 10% of classified DRAM fills; check first-touch and NUMA placement")
    if summary.l2_miss and summary.l3_or_same_ccx == 0 and summary.dram_total == 0:
        summary.notes.append("L2 misses are visible but demand-fill source counters are absent from this run")
    return summary


def summarize_pipeline(counts: Sequence[CountResult], pipeline_width: int) -> PipelineSummary | None:
    values = _counts_by_name(counts)
    cycles = values.get("cycles", 0)
    instructions = values.get("instructions", 0)
    if cycles <= 0 or pipeline_width <= 0:
        return None
    slots = cycles * pipeline_width
    summary = PipelineSummary(ipc=instructions / cycles if cycles else 0.0)
    if slots <= 0:
        return summary
    summary.frontend_bound = values.get("frontend_slots", 0) / slots
    summary.backend_bound = values.get("backend_slots", 0) / slots
    summary.smt_contention = values.get("smt_slots", 0) / slots
    if values.get("ops", 0):
        summary.retiring = values["ops"] / slots
    if values.get("dispatched_ops", 0) and values.get("ops", 0):
        summary.bad_speculation = max(values["dispatched_ops"] - values["ops"], 0) / slots
    if summary.ipc < 1.0:
        summary.notes.append("IPC below 1.0; correlate cache-source counters with IBS Op samples")
    return summary

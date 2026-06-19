#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

"""FreeBSD event specifiers for AMD Zen core PMCs."""

from dataclasses import dataclass
import warnings
from typing import Optional

from freebsd_cache_hotspot.topology import zen_generation


@dataclass(frozen=True, slots=True)
class FreeBsdAmdEvent:
    key: str
    freebsd_spec: str
    event_select: int
    unit_mask: int
    description: str

    @property
    def perfevtsel_raw(self) -> int:
        """Encoded as AMD PerfEvtSel low bits: EventSel[7:0], UnitMask[15:8],
        EventSel[11:8] in [35:32]. Sanity-check field, not what hwpmc writes."""
        return (
            (self.event_select & 0xFF)
            | ((self.unit_mask & 0xFF) << 8)
            | (((self.event_select >> 8) & 0xF) << 32)
        )


ZEN4_CORE_EVENTS = [
    FreeBsdAmdEvent("cycles", "ls_not_halted_cyc", 0x076, 0x00, "Core cycles not in halt"),
    FreeBsdAmdEvent("instructions", "ex_ret_instr", 0x0C0, 0x00, "Retired instructions"),
    FreeBsdAmdEvent("ops", "ex_ret_ops", 0x0C1, 0x00, "Retired macro-ops"),
    FreeBsdAmdEvent("branches", "ex_ret_brn", 0x0C2, 0x00, "Retired branches"),
    FreeBsdAmdEvent("branch_mispredicts", "ex_ret_brn_misp", 0x0C3, 0x00, "Retired branch mispredicts"),
    FreeBsdAmdEvent("dc_fill_l2", "ls_dmnd_fills_from_sys.local_l2", 0x043, 0x01, "Demand DC fills from local L2"),
    FreeBsdAmdEvent("dc_fill_l3", "ls_dmnd_fills_from_sys.local_ccx", 0x043, 0x02, "Demand DC fills from L3/same CCX"),
    FreeBsdAmdEvent("dc_fill_near_cache", "ls_dmnd_fills_from_sys.near_cache", 0x043, 0x04, "Demand DC fills from near cache"),
    FreeBsdAmdEvent("dc_fill_near_dram", "ls_dmnd_fills_from_sys.dram_io_near", 0x043, 0x08, "Demand DC fills from near DRAM/MMIO"),
    FreeBsdAmdEvent("dc_fill_far_cache", "ls_dmnd_fills_from_sys.far_cache", 0x043, 0x10, "Demand DC fills from far cache"),
    FreeBsdAmdEvent("dc_fill_far_dram", "ls_dmnd_fills_from_sys.dram_io_far", 0x043, 0x40, "Demand DC fills from far DRAM/MMIO"),
    FreeBsdAmdEvent("dc_fill_ext_mem", "ls_dmnd_fills_from_sys.alternate_memories", 0x043, 0x80, "Demand DC fills from extension memory"),
    FreeBsdAmdEvent("dc_fill_all", "ls_dmnd_fills_from_sys.all", 0x043, 0xFF, "Demand DC fills from all sources"),
    FreeBsdAmdEvent("l2_miss", "l2_cache_req_stat.ls_rd_blk_c", 0x064, 0x08, "Core-to-L2 data read miss"),
    FreeBsdAmdEvent("l2_access", "l2_cache_req_stat.dc_access_in_l2", 0x064, 0xF8, "Core-to-L2 data access"),
    FreeBsdAmdEvent("dtlb_miss", "ls_l1_d_tlb_miss.all", 0x045, 0xFF, "L1 DTLB misses, all page sizes"),
    FreeBsdAmdEvent("frontend_slots", "de_no_dispatch_per_slot.no_ops_from_frontend", 0x1A0, 0x01, "Empty dispatch slots: frontend"),
    FreeBsdAmdEvent("backend_slots", "de_no_dispatch_per_slot.backend_stalls", 0x1A0, 0x1E, "Empty dispatch slots: backend"),
    FreeBsdAmdEvent("smt_slots", "de_no_dispatch_per_slot.smt_contention", 0x1A0, 0x60, "Empty dispatch slots: SMT contention"),
    FreeBsdAmdEvent("dispatched_ops", "de_src_op_disp.all", 0x0AA, 0x07, "Ops dispatched from any source"),
]

PORTABLE_EVENTS = [
    FreeBsdAmdEvent("cycles", "unhalted-cycles", 0x076, 0x00, "Core cycles not in halt"),
    FreeBsdAmdEvent("instructions", "instructions", 0x0C0, 0x00, "Retired instructions"),
    FreeBsdAmdEvent("branches", "branches", 0x0C2, 0x00, "Retired branches"),
    FreeBsdAmdEvent("branch_mispredicts", "branch-mispredicts", 0x0C3, 0x00, "Retired branch mispredicts"),
]

DEFAULT_PROFILE = ["cycles", "instructions", "dc_fill_l3", "dc_fill_near_dram", "dc_fill_far_dram", "l2_miss"]
TOPDOWN_PROFILE = ["cycles", "ops", "frontend_slots", "backend_slots", "smt_slots", "dispatched_ops"]


def events_for_cpu(family: int, model: int) -> dict[str, FreeBsdAmdEvent]:
    gen = zen_generation(family, model)
    if gen == "Zen 4":
        return {event.key: event for event in ZEN4_CORE_EVENTS}
    if gen in {"Zen 5", "Zen 6"}:
        warnings.warn(
            f"{gen} detected; using only FreeBSD portable aliases until Zen 5/6 FreeBSD event tables are validated here.",
            UserWarning,
            stacklevel=2,
        )
        return {event.key: event for event in PORTABLE_EVENTS}
    warnings.warn(
        f"{gen or 'unknown AMD generation'} detected; using conservative FreeBSD aliases.",
        UserWarning,
        stacklevel=2,
    )
    return {event.key: event for event in PORTABLE_EVENTS}


def resolve_event(name: str, family: int, model: int) -> Optional[FreeBsdAmdEvent]:
    events = events_for_cpu(family, model)
    if name in events:
        return events[name]
    for event in events.values():
        if name == event.freebsd_spec:
            return event
    return None


def default_events(family: int, model: int, *, topdown: bool = False) -> list[str]:
    names = TOPDOWN_PROFILE if topdown else DEFAULT_PROFILE
    events = events_for_cpu(family, model)
    selected = [name for name in names if name in events]
    if selected:
        return selected
    return [name for name in ("cycles", "instructions") if name in events]

#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

"""AMD CPUID and FreeBSD hwpmc capability discovery."""

import os
import struct
import subprocess
from dataclasses import dataclass, field
from typing import Optional

from freebsd_cache_hotspot import _fbcacheprobe as _fb


ZEN1_MODELS = ((0x01, 0x0F),)
ZEN1_APU_MODELS = ((0x11, 0x1F),)
ZEN2_MODELS = ((0x31, 0x3F), (0x60, 0x7F), (0x90, 0xAF))
ZEN3_MODELS = ((0x00, 0x0F), (0x20, 0x2F), (0x50, 0x5F))
ZEN3PLUS_MODELS = ((0x40, 0x4F),)
ZEN4_MODELS = ((0x10, 0x1F), (0x60, 0x7F), (0xA0, 0xAF))
ZEN5_MODELS = ((0x00, 0x4F), (0x60, 0x7F))
ZEN6_MODELS = ((0x50, 0x5F), (0x80, 0xAF), (0xC0, 0xCF))


def _in_ranges(value: int, ranges: tuple[tuple[int, int], ...]) -> bool:
    return any(lo <= value <= hi for lo, hi in ranges)


def decode_family_model(eax: int) -> tuple[int, int, int]:
    stepping = eax & 0xF
    base_model = (eax >> 4) & 0xF
    base_family = (eax >> 8) & 0xF
    ext_model = (eax >> 16) & 0xF
    ext_family = (eax >> 20) & 0xFF
    family = base_family + ext_family if base_family == 0xF else base_family
    model = ((ext_model << 4) | base_model) if base_family in (0x6, 0xF) else base_model
    return family, model, stepping


def zen_generation(family: int, model: int) -> Optional[str]:
    if family == 0x17:
        if _in_ranges(model, ZEN2_MODELS):
            return "Zen 2"
        if _in_ranges(model, ZEN1_MODELS) or _in_ranges(model, ZEN1_APU_MODELS):
            return "Zen 1 / Zen+"
        return None
    if family == 0x19:
        if _in_ranges(model, ZEN4_MODELS):
            return "Zen 4"
        if _in_ranges(model, ZEN3PLUS_MODELS):
            return "Zen 3+"
        if _in_ranges(model, ZEN3_MODELS):
            return "Zen 3"
        return None
    if family == 0x1A:
        if _in_ranges(model, ZEN6_MODELS):
            return "Zen 6"
        if _in_ranges(model, ZEN5_MODELS):
            return "Zen 5"
        return None
    return None


def _cpuid_max(leaf: int) -> int:
    eax, _, _, _ = _fb.cpuid(leaf)
    return eax


def _sysctl(name: str) -> str:
    try:
        return subprocess.check_output(["sysctl", "-n", name], stderr=subprocess.DEVNULL, timeout=2).decode().strip()
    except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return ""


@dataclass(frozen=True, slots=True)
class CacheLevel:
    level: int
    type: str
    size_kb: int
    line_size: int
    ways: int
    sets: int
    shared_threads: int


@dataclass(frozen=True, slots=True)
class CpuIdentity:
    family: int
    model: int
    stepping: int
    vendor: str
    brand: str
    physical_addr_bits: int
    virtual_addr_bits: int

    @property
    def generation(self) -> str:
        if self.vendor != "AuthenticAMD":
            return "non-AMD"
        return zen_generation(self.family, self.model) or f"Unknown AMD family {self.family:#x} model {self.model:#x}"

    @property
    def pipeline_width(self) -> int:
        gen = zen_generation(self.family, self.model)
        if gen in {"Zen 1 / Zen+", "Zen 2", "Zen 3", "Zen 3+", "Zen 4"}:
            return 6
        if gen in {"Zen 5", "Zen 6"}:
            return 8
        return 0


@dataclass(slots=True)
class PmcCapabilities:
    perfmon_v2: bool = False
    core_counters: int = 0
    df_counters: int = 0
    lbr_v2_depth: int = 0
    active_umc_mask: int = 0
    ibs_fetch: bool = False
    ibs_op: bool = False
    ibs_op_data4: bool = False
    ibs_zen4_ext: bool = False
    ibs_ldlat: bool = False
    ibs_dtlb_pgsize: bool = False
    hwpmc_available: bool = False
    hwpmc_info: dict = field(default_factory=dict)


@dataclass(slots=True)
class Topology:
    cpu: CpuIdentity = field(init=False)
    pmc: PmcCapabilities = field(init=False)
    caches: list[CacheLevel] = field(default_factory=list, init=False)
    online_cpus: int = field(default=0, init=False)
    numa_domains: int = field(default=1, init=False)
    tsc_khz: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        self.cpu = self._discover_cpu()
        self.pmc = self._discover_pmc()
        self.caches = self._discover_caches()
        self.online_cpus = self._discover_online_cpus()
        self.numa_domains = self._discover_numa_domains()
        self.tsc_khz = _fb.tsc_freq_khz()

    def _discover_cpu(self) -> CpuIdentity:
        max_basic = _cpuid_max(0x00000000)
        max_ext = _cpuid_max(0x80000000)
        if max_basic < 1:
            raise RuntimeError("CPUID leaf 0x00000001 is unavailable")
        eax1, _, _, _ = _fb.cpuid(0x00000001)
        family, model, stepping = decode_family_model(eax1)

        _, ebx0, ecx0, edx0 = _fb.cpuid(0x00000000)
        vendor = struct.pack("<III", ebx0, edx0, ecx0).decode("ascii", errors="replace")

        brand = _sysctl("hw.model")
        if not brand and max_ext >= 0x80000004:
            raw = b""
            for leaf in (0x80000002, 0x80000003, 0x80000004):
                raw += struct.pack("<IIII", *_fb.cpuid(leaf))
            brand = raw.decode("ascii", errors="replace").rstrip("\x00").strip()

        phys_bits = virt_bits = 0
        if max_ext >= 0x80000008:
            eax8, _, _, _ = _fb.cpuid(0x80000008)
            phys_bits = eax8 & 0xFF
            virt_bits = (eax8 >> 8) & 0xFF

        return CpuIdentity(family, model, stepping, vendor, brand, phys_bits, virt_bits)

    def _discover_pmc(self) -> PmcCapabilities:
        cap = PmcCapabilities()
        if self.cpu.vendor != "AuthenticAMD":
            return cap
        extmax = _cpuid_max(0x80000000)
        if extmax >= 0x80000022:
            eax22, ebx22, ecx22, _ = _fb.cpuid(0x80000022)
            cap.perfmon_v2 = bool(eax22 & 1)
            cap.core_counters = ebx22 & 0x0F
            cap.lbr_v2_depth = (ebx22 >> 4) & 0x3F
            cap.df_counters = (ebx22 >> 10) & 0x3F
            cap.active_umc_mask = ecx22
        if cap.core_counters == 0:
            cap.core_counters = 6
        if extmax >= 0x8000001B:
            eax1b, _, _, _ = _fb.cpuid(0x8000001B)
            cap.ibs_fetch = bool(eax1b & (1 << 1))
            cap.ibs_op = bool(eax1b & (1 << 2))
            cap.ibs_op_data4 = bool(eax1b & (1 << 10))
            cap.ibs_zen4_ext = bool(eax1b & (1 << 11))
            cap.ibs_ldlat = bool(eax1b & (1 << 12))
            cap.ibs_dtlb_pgsize = bool(eax1b & (1 << 19))
        cap.hwpmc_available = bool(_fb.pmc_available())
        if cap.hwpmc_available:
            try:
                _fb.pmc_init()
                cap.hwpmc_info = dict(_fb.pmc_cpuinfo())
            except OSError as exc:
                cap.hwpmc_available = False
                cap.hwpmc_info = {"error": str(exc)}
        return cap

    @staticmethod
    def _discover_caches() -> list[CacheLevel]:
        caches: list[CacheLevel] = []
        max_ext = _cpuid_max(0x80000000)
        if max_ext < 0x8000001D:
            return caches
        for subleaf in range(16):
            eax, ebx, ecx, _ = _fb.cpuid(0x8000001D, subleaf)
            ctype = eax & 0x1F
            if ctype == 0:
                break
            level = (eax >> 5) & 0x7
            line = (ebx & 0xFFF) + 1
            partitions = ((ebx >> 12) & 0x3FF) + 1
            ways = ((ebx >> 22) & 0x3FF) + 1
            sets = ecx + 1
            size_kb = (ways * partitions * line * sets) // 1024
            shared_threads = ((eax >> 14) & 0xFFF) + 1
            name = {1: "Data", 2: "Instruction", 3: "Unified"}.get(ctype, f"type{ctype}")
            caches.append(CacheLevel(level, name, size_kb, line, ways, sets, shared_threads))
        return caches

    @staticmethod
    def _discover_online_cpus() -> int:
        text = _sysctl("hw.ncpu")
        if text.isdigit():
            return int(text)
        return os.cpu_count() or 1

    @staticmethod
    def _discover_numa_domains() -> int:
        text = _sysctl("vm.ndomains")
        if text.isdigit() and int(text) > 0:
            return int(text)
        return 1

    def cache_size_kb(self, level: int, cache_type: str | None = None) -> int:
        for cache in self.caches:
            if cache.level == level and (cache_type is None or cache.type == cache_type):
                return cache.size_kb
        return 0


def fmt_kb(kb: int) -> str:
    if kb >= 1024 * 1024 and kb % (1024 * 1024) == 0:
        return f"{kb // (1024 * 1024)}G"
    if kb >= 1024 and kb % 1024 == 0:
        return f"{kb // 1024}M"
    if kb >= 1024:
        return f"{kb / 1024:.1f}M"
    return f"{kb}K"

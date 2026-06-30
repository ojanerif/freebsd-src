#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: davi.chavesazevedo@amd.com

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from types import ModuleType
import sys

import pytest

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(PACKAGE_ROOT) not in sys.path:
    sys.path.insert(0, str(PACKAGE_ROOT))


def _is_hotspot_module(name: str) -> bool:
    return name == "freebsd_cache_hotspot" or name.startswith("freebsd_cache_hotspot.")


def _fake_probe_module() -> ModuleType:
    probe = ModuleType("freebsd_cache_hotspot._fbcacheprobe")
    probe.CACHE_LINE_SIZE = 64
    probe.cpuid = lambda leaf, subleaf=0: (0, 0, 0, 0)
    probe.pmc_available = lambda: False
    probe.pmc_cpuinfo = dict
    probe.tsc_freq_khz = lambda: 0
    return probe


@contextmanager
def _hotspot_imports_with_fake_probe() -> Iterator[None]:
    saved = {
        name: sys.modules[name]
        for name in list(sys.modules)
        if _is_hotspot_module(name)
    }
    for name in saved:
        sys.modules.pop(name, None)
    sys.modules["freebsd_cache_hotspot._fbcacheprobe"] = _fake_probe_module()
    try:
        yield
    finally:
        for name in [
            module_name
            for module_name in list(sys.modules)
            if _is_hotspot_module(module_name)
        ]:
            sys.modules.pop(name, None)
        sys.modules.update(saved)


def test_decode_family_model_handles_extended_amd_family_and_model() -> None:
    with _hotspot_imports_with_fake_probe():
        from freebsd_cache_hotspot.topology import decode_family_model

        eax = (0xB << 20) | (0x5 << 16) | (0xF << 8) | (0x4 << 4) | 0x1

        assert decode_family_model(eax) == (0x1A, 0x54, 1)


def test_family_1a_unknown_gap_stays_unknown_without_validation() -> None:
    with _hotspot_imports_with_fake_probe():
        from freebsd_cache_hotspot.topology import CpuIdentity, zen_generation

        gap_identity = CpuIdentity(0x1A, 0x30, 0, "AuthenticAMD", "synthetic", 0, 0)
        future_gap_identity = CpuIdentity(0x1A, 0xB0, 0, "AuthenticAMD", "synthetic", 0, 0)

        assert zen_generation(0x1A, 0x30) is None
        assert gap_identity.generation == "Unknown AMD family 0x1a model 0x30"
        assert gap_identity.pipeline_width == 0
        assert zen_generation(0x1A, 0xB0) is None
        assert future_gap_identity.generation == "Unknown AMD family 0x1a model 0xb0"
        assert future_gap_identity.pipeline_width == 0


def test_zen5_zen6_events_use_portable_aliases_until_validated() -> None:
    with _hotspot_imports_with_fake_probe():
        from freebsd_cache_hotspot.events import events_for_cpu

        with pytest.warns(UserWarning, match="Zen 5 detected"):
            zen5_events = events_for_cpu(0x1A, 0x44)
        with pytest.warns(UserWarning, match="Zen 6 detected"):
            zen6_events = events_for_cpu(0x1A, 0x54)

        assert set(zen5_events) == set(zen6_events) == {
            "cycles",
            "instructions",
            "branches",
            "branch_mispredicts",
        }
        assert "backend_slots" not in zen5_events
        assert "dc_fill_l3" not in zen6_events


def test_ibs_ldlat_rejection_names_cpuid_capability_not_generation(monkeypatch: pytest.MonkeyPatch) -> None:
    with _hotspot_imports_with_fake_probe():
        from freebsd_cache_hotspot import __main__ as cli
        from freebsd_cache_hotspot.topology import PmcCapabilities

        class FakeTopology:
            def __init__(self) -> None:
                self.pmc = PmcCapabilities(ibs_op=True, ibs_ldlat=False)

        parser = cli.build_parser()
        args = parser.parse_args(["ibs", "--ldlat", "256", "--", "/bin/true"])
        monkeypatch.setattr(cli, "Topology", FakeTopology)

        with pytest.raises(SystemExit) as excinfo:
            cli.cmd_ibs(args)

        message = str(excinfo.value)
        assert "CPUID 0x8000001B EAX[12]" in message
        assert "Zen 5 only" not in message

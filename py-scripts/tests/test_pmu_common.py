#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: davi.chavesazevedo@amd.com

from __future__ import annotations

import hashlib
import math
import sys
from pathlib import Path

import pytest

PY_SCRIPTS = Path(__file__).resolve().parents[1]
if str(PY_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(PY_SCRIPTS))


def test_parse_hwpmc_cpuid_decodes_genoa_string_without_hardware_probe() -> None:
    from pmu_common.amd_zen import parse_hwpmc_cpuid

    result = parse_hwpmc_cpuid("AuthenticAMD-25-11-1")

    assert result["valid"] is True
    assert result["family"] == 0x19
    assert result["family_hex"] == "0x19"
    assert result["model"] == 0x11
    assert result["model_hex"] == "0x11"
    assert result["generation"] == "Zen 4"
    assert result["codename"] == "Genoa/Bergamo/Siena"
    assert result["pipeline_width"] == 6


@pytest.mark.parametrize(
    ("cpuid", "generation", "pipeline_width"),
    [
        ("AuthenticAMD-26-4f-0", "Zen 5", 8),
        ("AuthenticAMD-26-30-0", "unknown AMD", 0),
        ("AuthenticAMD-26-50-0", "Zen 6", 8),
        ("AuthenticAMD-26-af-0", "Zen 6", 8),
        ("AuthenticAMD-26-b0-0", "unknown AMD", 0),
        ("AuthenticAMD-26-c0-0", "Zen 6", 8),
    ],
)
def test_parse_hwpmc_cpuid_keeps_family_1a_model_boundaries_explicit(
    cpuid: str,
    generation: str,
    pipeline_width: int,
) -> None:
    from pmu_common.amd_zen import parse_hwpmc_cpuid

    result = parse_hwpmc_cpuid(cpuid)

    assert result["valid"] is True
    assert result["family"] == 0x1A
    assert result["generation"] == generation
    assert result["pipeline_width"] == pipeline_width


def test_parse_hwpmc_cpuid_marks_non_amd_and_malformed_strings() -> None:
    from pmu_common.amd_zen import parse_hwpmc_cpuid

    non_amd = parse_hwpmc_cpuid("GenuineIntel-6-8f-8")
    malformed = parse_hwpmc_cpuid("AuthenticAMD-26-nothex-0")

    assert non_amd["valid"] is True
    assert non_amd["generation"] == "non-AMD"
    assert non_amd["ppr"] == "not applicable"
    assert malformed["valid"] is False
    assert malformed["generation"] == "unknown"


def test_percentile_interpolates_empty_and_invalid_inputs() -> None:
    from pmu_common.metrics import percentile

    assert percentile([], 0.50) is None
    assert percentile([42.0], 0.95) == 42.0
    assert percentile([0.0, 10.0], 0.25) == 2.5
    assert percentile([100.0, 0.0, 50.0], 0.50) == 50.0
    with pytest.raises(ValueError, match="pct must be in the range"):
        percentile([1.0], math.inf)


def test_sudo_config_builds_argv_without_shell_strings(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import pmu_common.command as command

    monkeypatch.setattr(command, "is_root", lambda: False)

    sudo = command.SudoConfig(
        use_sudo=True,
        sudo_cmd="/usr/bin/sudo",
        non_interactive=True,
    )
    no_sudo = command.SudoConfig(use_sudo=False)

    assert sudo.apply(["pmcstat", "-L"]) == [
        "/usr/bin/sudo",
        "-n",
        "pmcstat",
        "-L",
    ]
    assert no_sudo.apply([Path("/bin/true")]) == ["/bin/true"]


def test_atomic_write_text_round_trips_and_hashes_file(tmp_path: Path) -> None:
    from pmu_common.fs import atomic_write_text, read_text_file, sha256_file

    target = tmp_path / "nested" / "sample.txt"

    atomic_write_text(target, "pmu\n")

    assert read_text_file(target) == "pmu\n"
    assert sha256_file(target) == hashlib.sha256(b"pmu\n").hexdigest()
    assert read_text_file(tmp_path / "missing.txt") == ""
    assert sha256_file(None) is None

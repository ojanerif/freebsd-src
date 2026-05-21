#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Parse FreeBSD hwpmc CPUID strings and map AMD family/model to Zen metadata.

"""AMD Zen generation helpers for FreeBSD hwpmc CPUID strings.

Family 1Ah contains both Zen 5 and Zen 6; model range is mandatory.
"""

from __future__ import annotations

import re
from typing import Any, Dict, Tuple


CPUID_RE = re.compile(
    r"^(?P<vendor>[^-]+)-"
    r"(?P<family>[0-9]+)-"
    r"(?P<model>[0-9A-Fa-f]+)-"
    r"(?P<stepping>[0-9]+)$"
)


def map_amd_generation(family: int, model: int) -> Tuple[str, str, str, int]:
    """Return generation, codename, PPR publication, and pipeline width."""
    if family == 0x17:
        if 0x01 <= model <= 0x0F:
            return (
                "Zen 1 / Zen+",
                "Naples/Summit Ridge/Pinnacle Ridge",
                "54945",
                6,
            )
        if 0x11 <= model <= 0x1F:
            return ("Zen 1 / Zen+ APU", "Raven Ridge/Picasso", "55570", 6)
        if 0x31 <= model <= 0x3F:
            return ("Zen 2", "Rome/Castle Peak", "55898", 6)
        if 0x60 <= model <= 0x6F:
            return ("Zen 2", "Renoir", "56243", 6)
        if 0x71 <= model <= 0x7F:
            return ("Zen 2", "Matisse", "56243", 6)
        if 0x90 <= model <= 0x9F:
            return ("Zen 2", "Van Gogh", "unknown", 6)
        if 0xA0 <= model <= 0xAF:
            return ("Zen 2", "Mendocino", "unknown", 6)
    if family == 0x19:
        if 0x00 <= model <= 0x0F:
            return ("Zen 3", "Milan", "56569", 6)
        if 0x10 <= model <= 0x1F:
            return ("Zen 4", "Genoa/Bergamo/Siena", "57228", 6)
        if 0x20 <= model <= 0x2F:
            return ("Zen 3", "Vermeer", "56214", 6)
        if 0x40 <= model <= 0x4F:
            return ("Zen 3+", "Rembrandt", "56243", 6)
        if 0x50 <= model <= 0x5F:
            return ("Zen 3", "Cezanne", "56243", 6)
        if 0x60 <= model <= 0x6F:
            return ("Zen 4", "Raphael", "56713", 6)
        if 0x70 <= model <= 0x7F:
            return ("Zen 4", "Phoenix", "56713", 6)
        if 0xA0 <= model <= 0xAF:
            return ("Zen 4", "Genoa SP5/SP6", "57228", 6)
    if family == 0x1A:
        if 0x00 <= model <= 0x0F:
            return ("Zen 5", "Turin", "57238/58550", 8)
        if 0x10 <= model <= 0x1F:
            return ("Zen 5c", "Turin Dense", "58730", 8)
        if 0x20 <= model <= 0x2F:
            return ("Zen 5", "Strix Point", "57883", 8)
        if 0x40 <= model <= 0x4F:
            return ("Zen 5", "Eldora/Granite Ridge", "57930", 8)
        if 0x50 <= model <= 0x5F:
            return ("Zen 6", "Venice", "69163", 8)
        if 0x60 <= model <= 0x6F:
            return ("Zen 5", "Krackan Point", "unknown", 8)
        if 0x70 <= model <= 0x7F:
            return ("Zen 5", "Strix Halo", "unknown", 8)
        if 0x80 <= model <= 0xAF:
            return ("Zen 6", "future server", "unknown", 8)
        if 0xC0 <= model <= 0xCF:
            return ("Zen 6", "future", "unknown", 8)
    if family > 0x1A:
        return ("future AMD", "unknown", "consult PPR", 0)
    return ("unknown AMD", "unknown", "consult PPR", 0)


def parse_hwpmc_cpuid(cpuid: str) -> Dict[str, Any]:
    """Parse kern.hwpmc.cpuid: vendor-decimal_family-hex_model-stepping."""
    match = CPUID_RE.match(cpuid.strip())
    if match is None:
        return {
            "raw": cpuid.strip(),
            "valid": False,
            "vendor": "unknown",
            "family": None,
            "family_hex": None,
            "model": None,
            "model_hex": None,
            "stepping": None,
            "generation": "unknown",
            "codename": "unknown",
            "ppr": "unknown",
            "pipeline_width": 0,
        }

    vendor = match.group("vendor")
    family = int(match.group("family"), 10)
    model = int(match.group("model"), 16)
    stepping = int(match.group("stepping"), 10)
    generation = "unknown"
    codename = "unknown"
    ppr = "unknown"
    pipeline_width = 0
    if vendor == "AuthenticAMD":
        generation, codename, ppr, pipeline_width = map_amd_generation(family, model)
    else:
        generation = "non-AMD"
        codename = "unknown"
        ppr = "not applicable"
        pipeline_width = 0
    return {
        "raw": cpuid.strip(),
        "valid": True,
        "vendor": vendor,
        "family": family,
        "family_hex": f"0x{family:02x}",
        "model": model,
        "model_hex": f"0x{model:02x}",
        "stepping": stepping,
        "generation": generation,
        "codename": codename,
        "ppr": ppr,
        "pipeline_width": pipeline_width,
    }

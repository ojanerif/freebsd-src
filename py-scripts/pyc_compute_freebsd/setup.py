#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

"""Build configuration for the FreeBSD pyc_compute port."""

from __future__ import annotations

import os
import platform
from setuptools import Extension, setup


def _selected_march() -> str:
    return os.environ.get("FCH_MARCH", "-march=x86-64")


is_freebsd = platform.system() == "FreeBSD"

extra_compile_args = [
    "-O3",
    _selected_march(),
    "-Wall",
    "-Wextra",
    "-Werror=implicit-function-declaration",
    "-Wno-missing-field-initializers",
    "-std=c11",
    "-D_DEFAULT_SOURCE",
]

if is_freebsd:
    extra_compile_args.append("-D_FREEBSD_CACHE_HOTSPOT")

native = Extension(
    "freebsd_cache_hotspot._fbcacheprobe",
    sources=["ext/_fbcacheprobe.c"],
    libraries=["pmc", "m"] if is_freebsd else ["m"],
    extra_compile_args=extra_compile_args,
)

setup(ext_modules=[native])

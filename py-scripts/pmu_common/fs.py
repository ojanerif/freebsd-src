#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Filesystem helpers for atomic output, checksums, and compact raw text reads.

"""Atomic output and file-inspection helpers for PMU collection data."""

from __future__ import annotations

import contextlib
import hashlib
import os
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Optional, TextIO


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def read_text_file(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def fsync_directory(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return

    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_write_text(path: Path, data: str) -> None:
    """Atomically write text with file fsync and parent-directory fsync."""
    atomic_write_file(path, lambda fp: fp.write(data))


def atomic_write_file(path: Path, writer: Callable[[TextIO], object], suffix: str = "tmp") -> None:
    """Atomically write a text file through a same-directory temporary file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = unique_tmp_path(path, suffix)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    fd = None
    try:
        fd = os.open(tmp, flags, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as fp:
            fd = None
            writer(fp)
            fp.flush()
            os.fsync(fp.fileno())
        tmp.replace(path)
        fsync_directory(path.parent)
    except BaseException:
        if fd is not None:
            with contextlib.suppress(OSError):
                os.close(fd)
        with contextlib.suppress(OSError):
            tmp.unlink()
        raise


def unlink_suppress(path: Path) -> None:
    """Remove a path if present, ignoring ordinary unlink failures."""
    with contextlib.suppress(OSError):
        path.unlink()


def sha256_file(path: Optional[Path]) -> Optional[str]:
    if path is None or not path.exists():
        return None

    digest = hashlib.sha256()

    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            digest.update(chunk)

    return digest.hexdigest()


def unique_tmp_path(path: Path, suffix: str = "tmp") -> Path:
    token = uuid.uuid4().hex
    return path.with_name(f".{path.name}.{os.getpid()}.{token}.{suffix}")

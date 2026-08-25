#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Print application/ folder names that contain CMakeLists.txt (one per line)."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APPS = ROOT / "application"


def list_apps() -> list[str]:
    if not APPS.is_dir():
        return []
    names: list[str] = []
    for p in sorted(APPS.iterdir()):
        if p.is_dir() and (p / "CMakeLists.txt").is_file():
            names.append(p.name)
    return names


def main() -> int:
    apps = list_apps()
    if not apps:
        print("No applications found under application/*/CMakeLists.txt", file=sys.stderr)
        return 1
    for name in apps:
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

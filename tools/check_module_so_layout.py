#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Fail the build if a Store module .so has excessive PT_LOAD vaddr padding.

esp_elf_loader (segment mode, non-BUS_ADDRESS_MIRROR) warns when the vaddr gap
between consecutive PT_LOAD segments exceeds ADDR_OFFSET (0x400). Gaps of 4099
(0x1003) from default GNU ld page alignment have caused Instruction access faults
on ESP32-S31 when jumping into minihub_module_entry after dlopen.

Link with -Wl,-z,max-page-size=4 (see cmake/minihub_module_so.cmake).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# esp_elf.c: #define ADDR_OFFSET (0x400)
MAX_VADDR_GAP = 0x400


def pt_load_max_vaddr_gap(data: bytes) -> tuple[int, int]:
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")

    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]
    loads: list[tuple[int, int]] = []
    for i in range(e_phnum):
        off = e_phoff + i * 32
        p_type, _p_offset, p_vaddr, _p_paddr, _p_filesz, p_memsz, _p_flags, _p_align = struct.unpack_from(
            "<IIIIIIII", data, off
        )
        if p_type == 1:  # PT_LOAD
            loads.append((p_vaddr, p_memsz))

    if len(loads) < 2:
        return len(loads), 0

    max_gap = 0
    for i in range(len(loads) - 1):
        gap = loads[i + 1][0] - (loads[i][0] + loads[i][1])
        max_gap = max(max_gap, gap)
    return len(loads), max_gap


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("so_path", type=Path, help="libadapter_*.so to check")
    parser.add_argument(
        "--warn-only",
        action="store_true",
        help="Print warning but exit 0 (for transitional CI)",
    )
    args = parser.parse_args()

    path = args.so_path
    if not path.is_file():
        print(f"error: missing {path}", file=sys.stderr)
        return 1

    try:
        n_load, max_gap = pt_load_max_vaddr_gap(path.read_bytes())
    except ValueError as exc:
        print(f"error: {path.name}: {exc}", file=sys.stderr)
        return 1

    if max_gap > MAX_VADDR_GAP:
        msg = (
            f"{path.name}: PT_LOAD max vaddr gap {max_gap} (0x{max_gap:x}) exceeds "
            f"{MAX_VADDR_GAP}; elf_loader may fail on ESP32-S31. "
            f"Use -Wl,-z,max-page-size=4 (minihub_module_so.cmake)."
        )
        if args.warn_only:
            print(f"warning: {msg}", file=sys.stderr)
            return 0
        print(f"error: {msg}", file=sys.stderr)
        return 1

    print(f"ok: {path.name}: PT_LOAD segments={n_load} max_gap={max_gap}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

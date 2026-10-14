#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Bake the host ABI contract into a C header.

The header is the firmware copy used for offline checks. The store server
computes the same contract from ``minihub_module_abi.h`` and the export
allowlist at request time. This script does not write
``Stores/host_abi_versions.json`` or a csv beside it.

The per-symbol hash and ``abi_total_hash`` come from the
``minihub_module_host_api_t`` vtable plus the Host ELF export allowlist. See
abi_contract_lib for the hash rule.

  python3 tools/gen_abi_contract.py --out-header path/to/minihub_abi_symbols.gen.h
"""

from __future__ import annotations

import argparse
from pathlib import Path

from abi_contract_lib import (
    build_host_symbols,
    compute_abi_total_hash,
    parse_abi_version,
    write_baked_header,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HEADER = ROOT / "components/minihub_module/include/minihub_module_abi.h"
DEFAULT_ALLOWLIST = ROOT / "docs/module_sdk/host_export_allowlist.json"
DEFAULT_BAKED = ROOT / "components/minihub_module/include/minihub_abi_symbols.gen.h"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help="minihub_module_abi.h")
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST, help="host_export_allowlist.json")
    parser.add_argument("--abi-version", type=int, default=None, help="ABI version (default: parsed from --header)")
    parser.add_argument("--out-header", type=Path, default=DEFAULT_BAKED, help="baked C header path")
    args = parser.parse_args(argv)

    abi_version = args.abi_version if args.abi_version is not None else parse_abi_version(args.header)
    symbols = build_host_symbols(args.header, args.allowlist)
    abi_total = compute_abi_total_hash(symbols)

    args.out_header.parent.mkdir(parents=True, exist_ok=True)
    write_baked_header(args.out_header, abi_version, abi_total, symbols)

    print(f"  abi_version   : {abi_version}")
    print(f"  abi_total_hash: {abi_total}")
    print(f"  symbols       : {len(symbols)}")
    print(f"  baked header  : {args.out_header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

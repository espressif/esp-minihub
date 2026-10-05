#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Generate the host-side ABI contract.

Outputs (under the deploy root):

  Stores/host_abi_versions.json  -- server-side authoritative contract: an array
                                    of per-version entries
                                    [{version, abi_total_hash, symbols:{name:hash}}]
  Stores/host_abi_versions.csv   -- human-readable matrix (rows = api names,
                                    cols = abi versions, cell = hash) for review/diff
  <header out>                   -- baked C header for offline device-side checks

The contract is an *array* of abi versions so the host keeps the symbol hashes of
every published version. A device identifies itself by ``abi_version`` (and
optionally ``abi_total_hash``); the store server looks up that version's entry and
filters plugins by its symbol hashes. Bumping the host API only adds a new version
entry -- devices pinned to an older abi_version keep getting compatible plugins.

Re-running with the same ``--abi-version`` overwrites that entry; a new
``--abi-version`` is appended (history is preserved). The per-symbol hash and the
per-version ``abi_total_hash`` are derived from ``minihub_module_abi.h`` (the
``minihub_module_host_api_t`` vtable) plus the Host ELF export allowlist. See
abi_contract_lib for the hash rule.

  python3 tools/gen_abi_contract.py --deploy ~/workspace/minihub_store
"""

from __future__ import annotations

import argparse
import csv
import json
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
DEFAULT_STORES_ABI = "Stores/host_abi_versions.json"


def _load_prev_array(deploy: Path) -> list | None:
    """Read an existing contract as a list of version entries.

    Tolerates the legacy single-dict form by wrapping it.
    """
    path = deploy / DEFAULT_STORES_ABI
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(data, list):
        return data
    if isinstance(data, dict) and "abi_version" in data:
        return [data]
    return None


def _write_csv(csv_path: Path, entries: list[dict]) -> None:
    versions = [e["version"] for e in entries]
    names: list[str] = []
    seen: set[str] = set()
    for e in entries:
        for n in (e.get("symbols") or {}):
            if n not in seen:
                seen.add(n)
                names.append(n)
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["api_name"] + [f"v{v}" for v in versions])
        for n in names:
            row = [n]
            for v in versions:
                entry = next((e for e in entries if e["version"] == v), {}) or {}
                row.append((entry.get("symbols") or {}).get(n, ""))
            writer.writerow(row)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deploy", type=Path, default=None, help="deploy root (for Stores/host_abi_versions.json)")
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help="minihub_module_abi.h")
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST, help="host_export_allowlist.json")
    parser.add_argument("--abi-version", type=int, default=None, help="ABI version (default: parsed from --header)")
    parser.add_argument("--out-json", type=Path, default=None, help="host_abi_versions.json path")
    parser.add_argument("--out-csv", type=Path, default=None, help="host_abi_versions.csv path")
    parser.add_argument("--out-header", type=Path, default=DEFAULT_BAKED, help="baked C header path")
    args = parser.parse_args(argv)

    abi_version = args.abi_version if args.abi_version is not None else parse_abi_version(args.header)
    symbols = build_host_symbols(args.header, args.allowlist)
    abi_total = compute_abi_total_hash(symbols)
    entry = {
        "version": abi_version,
        "abi_total_hash": abi_total,
        "symbols": {n: h for n, h in sorted(symbols.items())},
    }

    # Merge into the version array, preserving every other published version.
    prev = _load_prev_array(args.deploy) if args.deploy else None
    entries = list(prev) if prev else []
    for i, e in enumerate(entries):
        if e.get("version") == abi_version:
            entries[i] = entry
            break
    else:
        entries.append(entry)
    entries.sort(key=lambda e: e.get("version", 0))

    out_json = args.out_json or (args.deploy / DEFAULT_STORES_ABI if args.deploy else None)
    if out_json:
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(json.dumps(entries, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        csv_path = args.out_csv or out_json.with_suffix(".csv")
        _write_csv(csv_path, entries)
        print(f"gen_abi_contract: {out_json}")
        print(f"  csv view      : {csv_path}")

    args.out_header.parent.mkdir(parents=True, exist_ok=True)
    write_baked_header(args.out_header, abi_version, abi_total, symbols)

    print(f"  abi_version   : {abi_version}")
    print(f"  abi_total_hash: {abi_total}")
    print(f"  symbols       : {len(symbols)}")
    print(f"  baked header  : {args.out_header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

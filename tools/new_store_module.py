#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Copy docs/module_sdk/templates/l{1,2,3} → components/store_modules/<id>/."""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "docs" / "module_sdk" / "templates"
STORE = ROOT / "components" / "store_modules"

ID_RE = re.compile(r"^[a-z][a-z0-9_]{0,30}$")


def replace_in_tree(dest: Path, module_id: str, name: str) -> None:
    for path in dest.rglob("*"):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        updated = text.replace("PLACEHOLDER_ID", module_id).replace("PLACEHOLDER_NAME", name)
        if updated != text:
            path.write_text(updated, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Scaffold a Store module from L1/L2/L3 templates.")
    parser.add_argument("--tier", required=True, choices=("l1", "l2", "l3"))
    parser.add_argument("--id", required=True, help="directory name / module.json id (lowercase, underscores)")
    parser.add_argument("--name", default="", help="display name (default: id)")
    parser.add_argument("--force", action="store_true", help="overwrite existing store_modules/<id>/")
    args = parser.parse_args()

    module_id = args.id.strip()
    if not ID_RE.match(module_id):
        print(
            f"invalid id {module_id!r}: use lowercase letter, then [a-z0-9_], max 31 chars (no hyphens)",
            file=sys.stderr,
        )
        return 2

    src = TEMPLATES / args.tier
    if not src.is_dir():
        print(f"missing template {src}", file=sys.stderr)
        return 1

    dest = STORE / module_id
    if dest.exists():
        if not args.force:
            print(f"{dest} already exists (pass --force to replace)", file=sys.stderr)
            return 1
        shutil.rmtree(dest)

    shutil.copytree(src, dest)
    name = args.name.strip() or module_id
    replace_in_tree(dest, module_id, name)
    print(f"created {dest} (tier {args.tier})")
    print(f"next: python3 tools/pack_store_module.py pack {module_id}")
    if args.tier == "l3":
        print("L3: build Host first so application/minihub/build/modules/libadapter_<id>.so exists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

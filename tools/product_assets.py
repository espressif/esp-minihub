#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Product-scoped assets: config JSON resolve + store_preinstall list.

Resolution for adapters.json / rules.json / scenes.json / packs / product_params:
  1. application/<app>/configs/<name>
  2. else configs/<name> (repo default)

Factory Store modules (modstore /store/modules/<id>/) come only from
application/<app>/manifest.yml → store_preinstall: [id, ...]
(empty / missing → no factory preinstall; module.json "preinstall" is ignored).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:
    print("PyYAML required: pip install pyyaml", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent

CONFIG_FILES = ("adapters.json", "rules.json", "scenes.json")
CONFIG_DIRS = ("packs", "product_params")


def load_app_manifest(repo: Path, app: str) -> dict[str, Any]:
    path = repo / "application" / app / "manifest.yml"
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    return data if isinstance(data, dict) else {}


def resolve_config_path(repo: Path, app: str, name: str) -> Path | None:
    """Return first existing path for a config file or directory name."""
    for base in (repo / "application" / app / "configs", repo / "configs"):
        candidate = base / name
        if candidate.exists():
            return candidate
    return None


def store_preinstall_ids(repo: Path, app: str) -> list[str]:
    raw = load_app_manifest(repo, app).get("store_preinstall")
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise SystemExit(
            f"application/{app}/manifest.yml: store_preinstall must be a list of module ids"
        )
    ids: list[str] = []
    sys.path.insert(0, str(ROOT / "tools"))
    from store_paths import module_source_dir  # noqa: E402

    for item in raw:
        if not isinstance(item, str) or not item.strip():
            raise SystemExit(f"application/{app}/manifest.yml: invalid store_preinstall entry {item!r}")
        mid = item.strip()
        mod_dir = module_source_dir(mid, root=repo)
        if (mod_dir / "module.json").is_file():
            if mid not in ids:
                ids.append(mid)
            continue
        raise SystemExit(
            f"store_preinstall: unknown module {mid!r} (missing {mod_dir / 'module.json'}; "
            f"not under examples/plugins or MINIHUB_PLUGIN_DIRS)"
        )
    return ids


def cmd_resolve(args: argparse.Namespace) -> int:
    path = resolve_config_path(args.repo, args.app, args.name)
    if path is None:
        print(f"missing config asset {args.name!r} for app={args.app}", file=sys.stderr)
        return 1
    print(path)
    return 0


def cmd_preinstall(args: argparse.Namespace) -> int:
    ids = store_preinstall_ids(args.repo, args.app)
    if args.format == "cmake":
        # CMake list: a;b;c
        print(";".join(ids))
    elif args.format == "json":
        print(json.dumps(ids))
    else:
        for mid in ids:
            print(mid)
    return 0


def cmd_seed_config(args: argparse.Namespace) -> int:
    """Copy resolved adapters/rules/scenes (+ optional dirs) into dest/config/."""
    dest = Path(args.dest)
    dest.mkdir(parents=True, exist_ok=True)
    for name in CONFIG_FILES:
        src = resolve_config_path(args.repo, args.app, name)
        if src is None or not src.is_file():
            print(f"skip {name} (not found)", file=sys.stderr)
            continue
        target = dest / name
        target.write_bytes(src.read_bytes())
        print(f"  → {target} (from {src})")
    for name in CONFIG_DIRS:
        src = resolve_config_path(args.repo, args.app, name)
        if src is None or not src.is_dir():
            continue
        out = dest / name
        out.mkdir(parents=True, exist_ok=True)
        for child in sorted(src.iterdir()):
            if child.is_file():
                (out / child.name).write_bytes(child.read_bytes())
        print(f"  → {out}/ (from {src})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo",
        type=Path,
        default=ROOT,
        help="MiniHub repo root",
    )
    parser.add_argument(
        "--app",
        default="",
        help="Product folder under application/ (default: MINIHUB_APP or minihub)",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_resolve = sub.add_parser("resolve", help="Print path of a config file/dir")
    p_resolve.add_argument("name", help="e.g. adapters.json or product_params")
    p_resolve.set_defaults(func=cmd_resolve)

    p_pre = sub.add_parser("store-preinstall", help="List factory-preinstall module ids")
    p_pre.add_argument(
        "--format",
        choices=("lines", "cmake", "json"),
        default="lines",
    )
    p_pre.set_defaults(func=cmd_preinstall)

    p_seed = sub.add_parser("seed-config", help="Copy resolved config JSON into dest/")
    p_seed.add_argument("dest", type=Path, help="Destination config directory")
    p_seed.set_defaults(func=cmd_seed_config)

    args = parser.parse_args()
    app = (args.app or "").strip()
    if not app:
        import os

        app = (os.environ.get("MINIHUB_APP") or "minihub").strip() or "minihub"
    args.app = app
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())

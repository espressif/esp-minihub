#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Discover and pack store_modules binary adapters into build MHMD .so + deploy .mhub zip."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import (  # noqa: E402
    ROOT,
    STORE_MODULES,
    module_deploy_dir,
    module_mhmd_path,
    resolve_deploy_dir,
)

STORE = STORE_MODULES


def app_name() -> str:
    return (os.environ.get("MINIHUB_APP", "minihub").strip() or "minihub")


def app_build_dir(*, root: Path = ROOT) -> Path:
    return root / "application" / app_name() / "build"


def resolve_modules_dir(*, root: Path = ROOT) -> Path:
    """Directory with libadapter_*.so (prefer current app, then minihub, then legacy)."""
    candidates = [
        app_build_dir(root=root) / "modules",
        root / "application" / "minihub" / "build" / "modules",
        root / "build" / "modules",
    ]
    for path in candidates:
        if path.is_dir() and any(path.glob("libadapter_*.so")):
            return path
    return candidates[0]


def list_binary_modules(store: Path = STORE) -> list[str]:
    """Dynamically list packable module directory names under store_modules."""
    ids: list[str] = []
    if not store.is_dir():
        return ids
    for d in sorted(store.iterdir()):
        if not d.is_dir():
            continue
        mj, cm = d / "module.json", d / "CMakeLists.txt"
        if not mj.is_file() or not cm.is_file():
            continue
        try:
            meta = json.loads(mj.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"WARN: skip {d.name}: bad module.json ({exc})", file=sys.stderr)
            continue
        if meta.get("config_only"):
            continue
        if meta.get("inject") and not meta.get("binary"):
            continue
        ids.append(d.name)
    return ids


def resolve_module_ids(requested: list[str] | None) -> list[str]:
    available = list_binary_modules()
    if not requested or requested == ["all"]:
        if not available:
            raise SystemExit(f"No binary modules found under {STORE}")
        return available
    unknown = [m for m in requested if m not in available]
    if unknown:
        raise SystemExit(
            f"Unknown or non-binary module(s): {', '.join(unknown)}\n"
            f"Available: {', '.join(available) or '(none)'}"
        )
    out: list[str] = []
    for m in requested:
        if m not in out:
            out.append(m)
    return out


def pack_one(
    module_id: str,
    *,
    root: Path = ROOT,
    deploy: Path | None = None,
) -> Path:
    deploy = resolve_deploy_dir(deploy)
    modules_dir = resolve_modules_dir(root=root)
    so_elf = modules_dir / f"libadapter_{module_id}.so"
    mhmd_build = module_mhmd_path(module_id, root=root)
    deploy_mod = module_deploy_dir(module_id, deploy)
    store_pkg = deploy_mod / f"{module_id}.mhub"

    if not so_elf.is_file():
        raise FileNotFoundError(
            f"Shared object not found: {so_elf}\n"
            f"(checked {modules_dir} and fallbacks under application/*/build/modules)\n"
            f"Run a full firmware build first (e.g. ./tools/build_firmware.py --app minihub build)"
        )

    try:
        from sync_store_catalog import read_adapter_version

        expected_av = read_adapter_version(STORE / module_id, module_id)
    except Exception:
        expected_av = ""
    if expected_av:
        so_text = so_elf.read_bytes()
        if expected_av.encode("utf-8") not in so_text:
            raise FileNotFoundError(
                f"{so_elf.name} is stale (missing adapter version {expected_av!r}).\n"
                f"Run a full firmware build first (e.g. ./tools/build_firmware.py build),\n"
                f"then re-run ./tools/build_firmware.py modules {module_id}"
            )

    mhmd_build.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [sys.executable, str(root / "tools" / "pack_store_module.py"), "mhmd", str(so_elf), str(mhmd_build)],
        cwd=str(root),
        check=True,
    )
    print(f"Built {module_id}: {mhmd_build} ({mhmd_build.stat().st_size} bytes)")
    subprocess.run(
        [
            sys.executable,
            str(root / "tools" / "pack_store_module.py"),
            module_id,
            "--deploy",
            str(deploy),
        ],
        cwd=str(root),
        check=True,
    )
    # Deploy is Store-facing: never leave MHMD .so beside the .mhub.
    stray = deploy_mod / f"{module_id}.so"
    if stray.is_file():
        stray.unlink()
    return store_pkg


def refresh_host_and_catalog(*, root: Path = ROOT, deploy: Path | None = None) -> None:
    deploy = resolve_deploy_dir(deploy)
    modules_dir = resolve_modules_dir(root=root)
    mhmd_dir = app_build_dir(root=root)
    subprocess.run(
        [
            sys.executable,
            str(root / "tools" / "gen_host_export_symbols.py"),
            "--modules-dir",
            str(modules_dir),
            "--mhmd-dir",
            str(mhmd_dir),
            "--store-dir",
            str(deploy),
            "--out",
            str(root / "components" / "minihub_module" / "host_elf_symbols_generated.c"),
        ],
        cwd=str(root),
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(root / "tools" / "sync_store_catalog.py"),
            "all",
            "--deploy",
            str(deploy),
        ],
        cwd=str(root),
        check=True,
    )


def pack_modules(
    requested: list[str] | None = None,
    *,
    root: Path = ROOT,
    deploy: Path | None = None,
) -> int:
    deploy = resolve_deploy_dir(deploy)
    ids = resolve_module_ids(requested)
    packing_all = not requested or requested == ["all"]
    print(f"Building modules: {' '.join(ids)}", flush=True)
    print(f"Deploy dir: {deploy}", flush=True)
    packed: list[str] = []
    skipped: list[tuple[str, str]] = []  # (id, reason)
    for mid in ids:
        modules_dir = resolve_modules_dir(root=root)
        so_elf = modules_dir / f"libadapter_{mid}.so"
        if not so_elf.is_file():
            reason = f"missing {so_elf.name} under {modules_dir}"
            if packing_all:
                print(f"SKIP {mid}: {reason}", flush=True)
                skipped.append((mid, reason))
                continue
            raise FileNotFoundError(
                f"Shared object not found: {so_elf}\n"
                f"(checked {modules_dir} and fallbacks under application/*/build/modules)\n"
                f"Run a full firmware build first (e.g. ./tools/build_firmware.py --app minihub build)"
            )
        pack_one(mid, root=root, deploy=deploy)
        packed.append(mid)

    def _print_summary() -> None:
        print(flush=True)
        print("======== .mhub pack summary ========", flush=True)
        print(f"Candidates: {len(ids)}", flush=True)
        print(f"Packed:     {len(packed)}", flush=True)
        print(f"Not packed: {len(skipped)}", flush=True)
        if packed:
            print("Packed .mhub:", flush=True)
            for mid in packed:
                pkg = module_deploy_dir(mid, deploy) / f"{mid}.mhub"
                if pkg.is_file():
                    print(f"  + {mid}.mhub  ({pkg.stat().st_size} bytes)  → {pkg}", flush=True)
                else:
                    print(f"  + {mid}.mhub  (path missing after pack)", flush=True)
        if skipped:
            print("Not packed:", flush=True)
            for mid, reason in skipped:
                print(f"  - {mid}  ({reason})", flush=True)
        print("====================================", flush=True)

    if not packed:
        _print_summary()
        print(
            "No libadapter_*.so available — skipping catalog refresh "
            "(use application/minihub build for full Store packages).",
            flush=True,
        )
        return 0
    refresh_host_and_catalog(root=root, deploy=deploy)
    _print_summary()
    print()
    print("Done. Deploy: full flash (writes modstore) or Web Store upgrade + Hub reload.")
    print(f"Store artifacts: {deploy}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pack store_modules binary adapters into MHMD .so (build/) + .mhub (deploy)."
    )
    parser.add_argument(
        "module_id",
        nargs="*",
        help="module id(s), or 'all' (default: all)",
    )
    parser.add_argument(
        "--deploy",
        type=Path,
        default=None,
        help="deploy root (default: MINIHUB_STORE_DEPLOY or ~/workspace/minihub_store)",
    )
    args = parser.parse_args(argv)
    requested = args.module_id if args.module_id else ["all"]
    try:
        return pack_modules(requested, deploy=args.deploy)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        return exc.returncode or 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Discover and pack Store modules into deploy .mhub (native MHMD + L2 JS)."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import (  # noqa: E402
    DEMO_PLUGINS,
    ROOT,
    iter_module_source_dirs,
    module_deploy_dir,
    module_mhmd_path,
    module_source_dir,
    resolve_deploy_dir,
)

STORE = DEMO_PLUGINS


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


def _load_manifest(d: Path) -> dict | None:
    mj = d / "module.json"
    if not mj.is_file():
        return None
    try:
        return json.loads(mj.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"WARN: skip {d.name}: bad module.json ({exc})", file=sys.stderr)
        return None


def module_pack_kind(meta: dict, d: Path) -> str | None:
    """Return ``js``, ``binary``, or ``None`` if not packable by this tool."""
    if meta.get("config_only"):
        return None
    runtime = meta.get("runtime") or ""
    if runtime == "js" or (
        runtime not in ("native", "hybrid", "pack")
        and (d / "main.js").is_file()
        and not (d / "CMakeLists.txt").is_file()
    ):
        if not (d / "main.js").is_file():
            return None
        return "js"
    if (d / "CMakeLists.txt").is_file():
        # L3 / hybrid — needs .so after firmware build
        if meta.get("inject") and not meta.get("binary"):
            return None
        return "binary"
    return None


def _iter_module_dirs(store: Path = STORE) -> list[Path]:
    """Plugin dirs with module.json: demos + MINIHUB_PLUGIN_DIRS.

    ``store`` is kept for callers that pass an alternate root; when it is the
    default demo tree, prefer ``iter_module_source_dirs`` so env trees are included.
    """
    if store.resolve() == DEMO_PLUGINS.resolve():
        return iter_module_source_dirs()
    dirs: list[Path] = []
    seen: set[Path] = set()
    if store.is_dir():
        for d in sorted(store.iterdir()):
            if not d.is_dir() or d.name.startswith(".") or d.name.startswith("_"):
                continue
            if (d / "module.json").is_file():
                key = d.resolve()
                if key not in seen:
                    seen.add(key)
                    dirs.append(d)
    return dirs


def list_packable_modules(store: Path = STORE) -> list[tuple[str, str]]:
    """Return ``[(module_id, kind), ...]`` where kind is ``js`` or ``binary``."""
    out: list[tuple[str, str]] = []
    seen: set[str] = set()
    for d in _iter_module_dirs(store):
        meta = _load_manifest(d)
        if not meta:
            continue
        kind = module_pack_kind(meta, d)
        if not kind:
            continue
        if d.name in seen:
            continue
        seen.add(d.name)
        out.append((d.name, kind))
    return out


def list_binary_modules(store: Path = STORE) -> list[str]:
    """Native/hybrid module ids (have CMakeLists). Kept for callers that need .so only."""
    return [mid for mid, kind in list_packable_modules(store) if kind == "binary"]


def resolve_module_ids(requested: list[str] | None) -> list[tuple[str, str]]:
    available = list_packable_modules()
    by_id = {mid: kind for mid, kind in available}
    if not requested or requested == ["all"]:
        if not available:
            raise SystemExit(
                f"No packable modules found under {STORE} / MINIHUB_PLUGIN_DIRS"
            )
        return available
    unknown = [m for m in requested if m not in by_id]
    if unknown:
        raise SystemExit(
            f"Unknown or not packable module(s): {', '.join(unknown)}\n"
            f"Available: {', '.join(by_id) or '(none)'}"
        )
    out: list[tuple[str, str]] = []
    for m in requested:
        if m not in [x[0] for x in out]:
            out.append((m, by_id[m]))
    return out


def pack_js_one(
    module_id: str,
    *,
    root: Path = ROOT,
    deploy: Path | None = None,
) -> Path:
    deploy = resolve_deploy_dir(deploy)
    deploy_mod = module_deploy_dir(module_id, deploy)
    store_pkg = deploy_mod / f"{module_id}.mhub"
    subprocess.run(
        [
            sys.executable,
            str(root / "tools" / "pack_store_module.py"),
            "pack",
            module_id,
            "--deploy",
            str(deploy),
        ],
        cwd=str(root),
        check=True,
    )
    return store_pkg


def pack_one(
    module_id: str,
    *,
    root: Path = ROOT,
    deploy: Path | None = None,
) -> Path:
    del deploy
    modules_dir = resolve_modules_dir(root=root)
    so_elf = modules_dir / f"libadapter_{module_id}.so"
    mhmd_build = module_mhmd_path(module_id, root=root)

    if not so_elf.is_file():
        raise FileNotFoundError(
            f"Shared object not found: {so_elf}\n"
            f"(checked {modules_dir} and fallbacks under application/*/build/modules)\n"
            "Compile it with ./tools/build_firmware.py module <id>"
        )

    try:
        from sync_store_catalog import read_adapter_version

        expected_av = read_adapter_version(module_source_dir(module_id, root=root), module_id)
    except Exception:
        expected_av = ""
    if expected_av:
        so_text = so_elf.read_bytes()
        if expected_av.encode("utf-8") not in so_text:
            raise FileNotFoundError(
                f"{so_elf.name} is stale (missing adapter version {expected_av!r}).\n"
                f"Rebuild that module, then pack it:\n"
                f"  ./tools/build_firmware.py module {module_id}\n"
                f"  ./tools/build_firmware.py pack {module_id}"
            )

    mhmd_build.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [sys.executable, str(root / "tools" / "pack_store_module.py"), "mhmd", str(so_elf), str(mhmd_build)],
        cwd=str(root),
        check=True,
    )
    print(f"Built {module_id}: {mhmd_build} ({mhmd_build.stat().st_size} bytes)")
    print(f"skip {module_id}: native module is wrapped into a build-local "
          f".mhub and is not published to the store (not left as a bare .so)")
    return mhmd_build


def host_elf_symbols_out(*, root: Path = ROOT) -> Path:
    """Build-tree copy; do not overwrite the committed components/… source file."""
    out = app_build_dir(root=root) / "gen" / "host_elf_symbols_generated.c"
    out.parent.mkdir(parents=True, exist_ok=True)
    return out


# L3 adapters are not built or published. gen_host_export_symbols.py scans
# libadapter_*.so and exits 1 when none exist, which fails a JS-only pack.
# Set this true again when an L3 .so is part of the pack.
REFRESH_HOST_ELF_SYMBOLS = False


def refresh_host_and_catalog(*, root: Path = ROOT, deploy: Path | None = None) -> None:
    deploy = resolve_deploy_dir(deploy)
    if REFRESH_HOST_ELF_SYMBOLS:
        modules_dir = resolve_modules_dir(root=root)
        mhmd_dir = app_build_dir(root=root)
        gen_out = host_elf_symbols_out(root=root)
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
                str(gen_out),
                "--skip-allowlist",
            ],
            cwd=str(root),
            check=True,
        )
    else:
        print("skip gen_host_export_symbols: L3 adapters are not built", flush=True)
    # Default catalog rewrite scans deploy .mhub only (no plugin sources).
    subprocess.run(
        [
            sys.executable,
            str(root / "tools" / "sync_store_catalog.py"),
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
    skip_native: bool = False,
) -> int:
    deploy = resolve_deploy_dir(deploy)
    items = resolve_module_ids(requested)
    packing_all = not requested or requested == ["all"]
    ids = [mid for mid, _ in items]
    print(f"Packing modules: {' '.join(ids)}", flush=True)
    print(f"Deploy dir: {deploy}", flush=True)
    packed: list[str] = []
    skipped: list[tuple[str, str]] = []  # (id, reason)
    for mid, kind in items:
        if kind != "js" and skip_native and packing_all:
            print(f"SKIP {mid}: native module", flush=True)
            skipped.append((mid, "native module"))
            continue
        if kind == "js":
            pack_js_one(mid, root=root, deploy=deploy)
            packed.append(mid)
            continue
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
                "Compile it with ./tools/build_firmware.py module <id>"
            )
        pack_one(mid, root=root, deploy=deploy)
        packed.append(mid)

    def _print_summary() -> None:
        print(flush=True)
        print("======== .mhub pack summary ========", flush=True)
        print(f"Candidates: {len(items)}", flush=True)
        print(f"Packed:     {len(packed)}", flush=True)
        print(f"Not packed: {len(skipped)}", flush=True)
        if packed:
            print("Packed .mhub:", flush=True)
            kind_by_id = {item: kind for item, kind in items}
            for mid in packed:
                if kind_by_id.get(mid) != "js":
                    print(f"  + {mid}  (native, not published to the store)", flush=True)
                    continue
                pkg = module_deploy_dir(mid, deploy) / f"{mid}.mhub"
                if pkg.is_file():
                    print(
                        f"  + {mid}.mhub  ({pkg.stat().st_size} bytes, js)  → {pkg}",
                        flush=True,
                    )
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
            "No modules packed — skipping catalog refresh "
            "(JS needs main.js; native needs libadapter_*.so after firmware build).",
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
        description="Pack Store modules into .mhub (L2 JS + L3/hybrid native)."
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
        help="deploy root (default: MINIHUB_PLUGINS_DEPLOY_DIR or ~/workspace/minihub_store)",
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

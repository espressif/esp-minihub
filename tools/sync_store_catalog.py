#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Rebuild deploy catalog.json from minihub_plugins/*/module.json; sync .hub packages."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import (  # noqa: E402
    ROOT,
    iter_module_source_dirs,
    module_deploy_dir,
    module_mhmd_path,
    module_source_dir,
    resolve_deploy_dir,
)


def iter_module_dirs() -> list[Path]:
    """Plugin sources for catalog: minihub_plugins + MINIHUB_PLUGIN_DIRS."""
    return iter_module_source_dirs(root=ROOT)


def read_adapter_version(module_dir: Path, module_id: str) -> str:
    candidates = [
        module_dir / f"{module_id}_adapter.c",
        *sorted(module_dir.glob("*_adapter.c")),
    ]
    for path in candidates:
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        match = re.search(r"\.version\s*=\s*\"([^\"]+)\"", text)
        if match:
            return match.group(1)
    return ""


def is_config_only(manifest: dict) -> bool:
    if manifest.get("config_only") is True:
        return True
    if manifest.get("runtime") == "pack":
        return True
    if manifest.get("runtime") in ("js", "native", "hybrid") or manifest.get("binary"):
        return False
    if manifest.get("inject") or manifest.get("requires"):
        return True
    return False


def is_hybrid_module(manifest: dict) -> bool:
    return manifest.get("runtime") == "hybrid"


def is_js_module(manifest: dict, module_dir: Path) -> bool:
    if is_hybrid_module(manifest):
        return False
    if manifest.get("runtime") == "js":
        return True
    if manifest.get("runtime") == "native":
        return False
    if (module_dir / "CMakeLists.txt").is_file():
        return False
    return (module_dir / "main.js").is_file()


def binary_size_from_mhub(pkg: Path, module_id: str) -> int | None:
    if not pkg.is_file():
        return None
    try:
        with zipfile.ZipFile(pkg) as zf:
            name = f"{module_id}.so"
            if name in zf.namelist():
                return zf.getinfo(name).file_size
            if "module.json" in zf.namelist():
                meta = json.loads(zf.read("module.json"))
                size = meta.get("binary_size")
                if isinstance(size, int):
                    return size
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError):
        return None
    return None


def catalog_entry(manifest: dict, module_dir: Path, deploy_mod: Path) -> dict:
    mid = manifest["id"]
    entry: dict = {
        "kind": "module",
        "id": mid,
        "name": manifest.get("name", mid),
        "version": manifest.get("version", "1.0.0"),
        "vendor": manifest.get("vendor", ""),
        "description": manifest.get("description") or manifest.get("comment", ""),
        "url": f"/files/{mid}.mhub",
    }
    adapter = manifest.get("adapter")
    if not adapter and isinstance(manifest.get("inject"), dict):
        adapter = manifest["inject"].get("adapter")
    if not adapter and isinstance(manifest.get("requires"), list) and manifest["requires"]:
        adapter = manifest["requires"][0]
    if adapter:
        entry["adapter"] = adapter

    pkg = deploy_mod / f"{mid}.mhub"
    if pkg.is_file():
        entry["size"] = pkg.stat().st_size

    so = module_mhmd_path(mid)
    if so.is_file():
        entry["binary_size"] = so.stat().st_size
    else:
        size = binary_size_from_mhub(pkg, mid)
        if size is not None:
            entry["binary_size"] = size
        elif isinstance(manifest.get("binary_size"), int):
            entry["binary_size"] = manifest["binary_size"]

    if is_config_only(manifest):
        entry["config_only"] = True
        entry["runtime"] = "pack"
    elif is_js_module(manifest, module_dir):
        entry["runtime"] = "js"
        if manifest.get("host_api") is not None:
            entry["host_api"] = manifest["host_api"]
        else:
            entry["host_api"] = 0
    elif is_hybrid_module(manifest):
        entry["runtime"] = "hybrid"
        entry_v = manifest.get("entry")
        if entry_v in ("native", "js"):
            entry["entry"] = entry_v
        if manifest.get("host_api") is not None:
            entry["host_api"] = manifest["host_api"]
        else:
            entry["host_api"] = 0
        av = manifest.get("adapter_version") or read_adapter_version(module_dir, mid)
        if av:
            entry["adapter_version"] = av
        if isinstance(manifest.get("ui"), dict):
            entry["ui"] = manifest["ui"]
        else:
            ui_name = manifest.get("ui_file") or "ui.json"
            ui_path = module_dir / ui_name
            if ui_path.is_file():
                try:
                    entry["ui"] = json.loads(ui_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    print(f"WARN: bad {ui_path}: {exc}", file=sys.stderr)
        if isinstance(manifest.get("capabilities"), list):
            entry["capabilities"] = manifest["capabilities"]
    else:
        entry["runtime"] = manifest.get("runtime") or "native"
        av = manifest.get("adapter_version") or read_adapter_version(module_dir, mid)
        if av:
            entry["adapter_version"] = av
        if isinstance(manifest.get("ui"), dict):
            entry["ui"] = manifest["ui"]
        else:
            ui_name = manifest.get("ui_file") or "ui.json"
            ui_path = module_dir / ui_name
            if ui_path.is_file():
                try:
                    entry["ui"] = json.loads(ui_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    print(f"WARN: bad {ui_path}: {exc}", file=sys.stderr)
        if isinstance(manifest.get("capabilities"), list):
            entry["capabilities"] = manifest["capabilities"]

    if is_js_module(manifest, module_dir):
        if isinstance(manifest.get("ui"), dict):
            entry["ui"] = manifest["ui"]
        elif (module_dir / "ui.json").is_file():
            try:
                entry["ui"] = json.loads((module_dir / "ui.json").read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
        if isinstance(manifest.get("capabilities"), list):
            entry["capabilities"] = manifest["capabilities"]

    return entry


def rebuild_catalog(deploy: Path) -> list[dict]:
    catalog: list[dict] = []
    for module_dir in iter_module_dirs():
        manifest = json.loads((module_dir / "module.json").read_text(encoding="utf-8"))
        if not manifest.get("id"):
            manifest["id"] = module_dir.name
        deploy_mod = module_deploy_dir(module_dir.name, deploy)
        catalog.append(catalog_entry(manifest, module_dir, deploy_mod))
    deploy.mkdir(parents=True, exist_ok=True)
    path = deploy / "catalog.json"
    path.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return catalog


def sync_module(module_id: str, deploy: Path) -> int:
    module_dir = module_source_dir(module_id)
    manifest_path = module_dir / "module.json"
    deploy_mod = module_deploy_dir(module_id, deploy)
    pkg = deploy_mod / f"{module_id}.mhub"
    if not manifest_path.is_file():
        print(f"missing {manifest_path}", file=sys.stderr)
        return 1

    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "pack_store_module.py"),
            module_id,
            "--deploy",
            str(deploy),
        ],
        cwd=str(ROOT),
        check=True,
    )
    if not pkg.is_file():
        print(f"pack_store_module did not produce {pkg}", file=sys.stderr)
        return 1

    print(f"sync_store_catalog: {module_id} pkg_size={pkg.stat().st_size} → {pkg}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "module_id",
        nargs="?",
        default="all",
        help="module id, or 'all' (default)",
    )
    parser.add_argument(
        "--deploy",
        type=Path,
        default=None,
        help="deploy root (default: MINIHUB_STORE_DEPLOY or ~/workspace/minihub_store)",
    )
    parser.add_argument(
        "--catalog-only",
        action="store_true",
        help="rewrite catalog.json sizes from existing deploy/*.mhub (no repack)",
    )
    args = parser.parse_args(argv)
    deploy = resolve_deploy_dir(args.deploy)
    arg = args.module_id

    if args.catalog_only:
        catalog = rebuild_catalog(deploy)
        print(
            f"sync_store_catalog: wrote {deploy / 'catalog.json'} "
            f"({len(catalog)} modules, catalog-only)"
        )
        return 0

    if arg in ("all", "--all", "-a"):
        for module_dir in iter_module_dirs():
            rc = sync_module(module_dir.name, deploy)
            if rc != 0:
                return rc
        catalog = rebuild_catalog(deploy)
        print(
            f"sync_store_catalog: wrote {deploy / 'catalog.json'} ({len(catalog)} modules)"
        )
        return 0

    rc = sync_module(arg, deploy)
    if rc != 0:
        return rc
    catalog = rebuild_catalog(deploy)
    print(
        f"sync_store_catalog: wrote {deploy / 'catalog.json'} ({len(catalog)} modules)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

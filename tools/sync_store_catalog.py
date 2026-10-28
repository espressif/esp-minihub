#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Write per-plugin ``catalog.json`` next to each packed ``.mhub``.

Every published module lives under one ``Stores/<bucket>/<id>/`` folder.
L1 and L2 use ``Common``; native and hybrid are not published at all
(``store_buckets_for_module()`` returns no bucket and a leftover folder is
deleted). ``targets`` is the chip filter for ``?chip=``.

Default (no args, or ``--from-store``): scan ``DEPLOY/Stores/<bucket>/<id>/<id>.mhub``
only — no plugin sources, no ``MINIHUB_PLUGIN_DIRS`` — and move a package
whose folder does not match its ``targets``. Pass a module id (or ``all``)
to pack from sources first, then rewrite those catalogs the same way.

``/api/catalog`` aggregates the per-plugin objects (one id once) at request
time, so no derived index file is written. The per-plugin ``catalog.json``
next to each ``.mhub`` is the only source of truth. Pass ``--aggregate PATH``
to emit one array at an explicit path (the device's bundled local store
needs such a seed; the deploy tree does not).
"""

from __future__ import annotations

import argparse
import hashlib
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
    iter_store_plugin_dirs,
    module_mhmd_path,
    module_source_dir,
    normalize_chip_folders,
    prune_plugin_dirs,
    resolve_deploy_dir,
)

# Native and hybrid modules link Host symbols and must carry an ``abi`` block
# (abi_version + the host symbol hashes this package links) for the server's
# compatibility check. JS and config-only (``pack``) modules are ABI-independent
# and omit the ``abi`` block entirely.
try:
    from abi_contract_lib import build_host_symbols, parse_abi_version, plugin_symbol_block
except Exception:  # pragma: no cover - optional
    build_host_symbols = parse_abi_version = plugin_symbol_block = None

_HOST_SYMBOLS: dict | None = None


def _host_symbols() -> dict:
    global _HOST_SYMBOLS
    if _HOST_SYMBOLS is None:
        if build_host_symbols is None:
            _HOST_SYMBOLS = {}
        else:
            try:
                _HOST_SYMBOLS = build_host_symbols()
            except Exception as exc:  # noqa: BLE001
                print(f"[abi] warn: cannot build host symbols: {exc}", file=sys.stderr)
                _HOST_SYMBOLS = {}
    return _HOST_SYMBOLS


def _abi_block(mhub: Path) -> dict | None:
    syms = _host_symbols()
    if not syms or plugin_symbol_block is None:
        return None
    block = plugin_symbol_block(mhub, syms) or {}
    version = parse_abi_version() if parse_abi_version else 1
    return {"abi_version": version, "symbols": block}


def iter_module_dirs() -> list[Path]:
    """Plugin sources for catalog: MINIHUB_PLUGIN_DIRS, else examples/plugins."""
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


# Same chip-folder string as Firmwares/<chip>/ and /api/firmware "targets".
# JS modules are not tied to one chip image, so an omitted list is ALL.
DEFAULT_PLUGIN_TARGETS = ["esp32s31"]
DEFAULT_JS_PLUGIN_TARGETS = ["ALL"]


def _normalize_target(item: str) -> str:
    text = item.strip()
    if text.upper() == "ALL" or text == "*":
        return "ALL"
    return text.lower()


def plugin_targets(manifest: dict, module_dir: Path | None = None) -> list[str]:
    """Chip list for catalog.json and the packaged module.json.

    ``targets`` is a list of lowercase chip folder names. A single string is
    wrapped. ``ALL`` stays ``ALL``. An omitted or empty value becomes ``ALL``
    for a JS module and ``esp32s31`` for every other module.
    """
    raw = manifest.get("targets")
    if isinstance(raw, str):
        raw = [raw]
    if isinstance(raw, list):
        out = [_normalize_target(item) for item in raw if isinstance(item, str) and item.strip()]
        if out:
            return out
    js = False
    if module_dir is not None:
        js = is_js_module(manifest, module_dir)
    elif manifest.get("runtime") == "js":
        js = True
    if js:
        return list(DEFAULT_JS_PLUGIN_TARGETS)
    return list(DEFAULT_PLUGIN_TARGETS)


def store_buckets_for_module(manifest: dict, module_dir: Path) -> list[str]:
    """L1 and L2 are stored under ``Stores/Common``.

    A native or hybrid module is not a store package: it gets no plugin
    folder, including ``Stores/Common`` and ``Stores/<chip>``.
    """
    if _module_is_native_package(manifest, module_dir):
        return []
    return ["Common"]


def _module_is_native_package(manifest: dict, module_dir: Path) -> bool:
    if is_hybrid_module(manifest) or manifest.get("runtime") == "native":
        return True
    if manifest.get("binary"):
        return True
    if is_config_only(manifest) or is_js_module(manifest, module_dir):
        return False
    return (module_dir / "CMakeLists.txt").is_file()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _dump(obj) -> str:
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


def catalog_entry(
    manifest: dict,
    module_dir: Path,
    deploy_mod: Path,
    *,
    bucket: str | None = None,
) -> dict:
    mid = manifest["id"]
    targets = plugin_targets(manifest, module_dir)
    if not bucket:
        chosen = store_buckets_for_module(manifest, module_dir)
        bucket = chosen[0] if chosen else ""
    entry: dict = {
        "type": "module",
        "id": mid,
        "name": manifest.get("name", mid),
        "version": manifest.get("version", "1.0.0"),
        "vendor": manifest.get("vendor", ""),
        "description": manifest.get("description") or manifest.get("comment", ""),
    }
    if bucket:
        entry["url"] = f"/files/Stores/{bucket}/{mid}/{mid}.mhub"
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
        entry["sha256"] = sha256_file(pkg)
        # JS and config-only (pack) modules are ABI-independent: skip the abi
        # block so the catalog states it plainly. Native/hybrid keep it.
        if not (is_js_module(manifest, module_dir) or is_config_only(manifest)):
            abi_block = _abi_block(pkg)
            if abi_block:
                entry["abi"] = abi_block

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
    else:
        entry["runtime"] = manifest.get("runtime") or "native"
        av = manifest.get("adapter_version") or read_adapter_version(module_dir, mid)
        if av:
            entry["adapter_version"] = av

    entry["targets"] = targets
    return entry


def _write_plugin_catalog(deploy_mod: Path, entry: dict) -> None:
    deploy_mod.mkdir(parents=True, exist_ok=True)
    (deploy_mod / "catalog.json").write_text(_dump(entry), encoding="utf-8")


def write_aggregate(path: Path, catalog: list[dict]) -> Path:
    """Write the derived array to an explicit path (for example a modstore seed).

    The deploy tree carries no derived index: ``/api/catalog`` aggregates the
    per-plugin ``catalog.json`` objects at request time. Callers that still need
    a single array (the device's bundled local store) ask for one here.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_dump(catalog), encoding="utf-8")
    return path


def _read_mhub(pkg: Path) -> dict | None:
    try:
        with zipfile.ZipFile(pkg) as zf:
            if "module.json" not in zf.namelist():
                print(f"WARN: {pkg} has no module.json", file=sys.stderr)
                return None
            manifest = json.loads(zf.read("module.json"))
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        print(f"WARN: skip {pkg}: {exc}", file=sys.stderr)
        return None
    if not isinstance(manifest, dict):
        return None
    return manifest


def _drop_unpublished_plugin(deploy: Path, module_id: str) -> None:
    """Remove an L3 package from every store folder. Do not recreate one."""
    prune_plugin_dirs(deploy, module_id, [])


def _place_catalogs(
    manifest: dict,
    module_dir: Path,
    deploy: Path,
    *,
    move_packages: bool,
) -> dict | None:
    """Write ``catalog.json`` in each bucket. Optionally move the ``.mhub``.

    L1/L2 use ``Stores/Common/<id>/``. A native or hybrid module has no bucket,
    so nothing is written and any package left from an earlier build is removed.
    """
    mid = manifest["id"]
    buckets = store_buckets_for_module(manifest, module_dir)
    if not buckets:
        if move_packages:
            _drop_unpublished_plugin(deploy, mid)
        return None
    dirs = [deploy / "Stores" / bucket / mid for bucket in buckets]
    src_pkg: Path | None = None
    blob: bytes | None = None
    if move_packages:
        for pkg in (
            child / f"{mid}.mhub"
            for child in iter_store_plugin_dirs(deploy)
            if child.name == mid
        ):
            if pkg.is_file():
                src_pkg = pkg
                blob = pkg.read_bytes()
                break
    primary: dict | None = None
    for bucket, deploy_mod in zip(buckets, dirs):
        if blob is not None:
            deploy_mod.mkdir(parents=True, exist_ok=True)
            dest_pkg = deploy_mod / f"{mid}.mhub"
            if src_pkg is None or dest_pkg.resolve() != src_pkg.resolve():
                dest_pkg.write_bytes(blob)
        entry = catalog_entry(manifest, module_dir, deploy_mod, bucket=bucket)
        pkg = deploy_mod / f"{mid}.mhub"
        packed = binary_size_from_mhub(pkg, mid)
        if packed is not None:
            entry["binary_size"] = packed
        _write_plugin_catalog(deploy_mod, entry)
        if primary is None:
            primary = entry
    if move_packages:
        prune_plugin_dirs(deploy, mid, buckets)
    return primary


def rebuild_catalog_from_store(deploy: Path) -> list[dict]:
    """Rewrite each plugin catalog from ``Stores/<bucket>/<id>/<id>.mhub``.

    Moves a package into the folders selected by its ``targets``. Does not
    need plugin sources.
    """
    normalize_chip_folders(deploy)
    stores = deploy / "Stores"
    if not stores.is_dir():
        raise FileNotFoundError(f"missing store dir {stores}")
    found: dict[str, Path] = {}
    for child in iter_store_plugin_dirs(deploy):
        pkg = child / f"{child.name}.mhub"
        if pkg.is_file() and child.name not in found:
            found[child.name] = pkg
    catalog: list[dict] = []
    for mid in sorted(found):
        manifest = _read_mhub(found[mid])
        if manifest is None:
            continue
        if not manifest.get("id"):
            manifest["id"] = mid
        entry = _place_catalogs(
            manifest,
            found[mid].parent,
            deploy,
            move_packages=True,
        )
        if entry is None:
            continue
        catalog.append(entry)
    return catalog


def rebuild_catalog(deploy: Path) -> list[dict]:
    catalog: list[dict] = []
    for module_dir in iter_module_dirs():
        manifest = json.loads((module_dir / "module.json").read_text(encoding="utf-8"))
        if not manifest.get("id"):
            manifest["id"] = module_dir.name
        entry = _place_catalogs(manifest, module_dir, deploy, move_packages=False)
        if entry is not None:
            catalog.append(entry)
    return catalog


def write_packed_catalog(module_id: str, deploy: Path) -> dict:
    """Write the per-plugin ``catalog.json`` beside its ``.mhub`` after pack.

    No derived aggregate is written; ``/api/catalog`` aggregates per-plugin
    catalogs at request time.
    """
    module_dir = module_source_dir(module_id)
    manifest_path = module_dir / "module.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"missing {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not manifest.get("id"):
        manifest["id"] = module_id
    entry = _place_catalogs(manifest, module_dir, deploy, move_packages=False)
    if entry is None:
        raise FileNotFoundError(f"no catalog for {module_id}")
    return entry


def sync_module(module_id: str, deploy: Path) -> int:
    module_dir = module_source_dir(module_id)
    manifest_path = module_dir / "module.json"
    if not manifest_path.is_file():
        print(f"missing {manifest_path}", file=sys.stderr)
        return 1
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    buckets = store_buckets_for_module(manifest, module_dir)
    if not buckets:
        print(f"skip {module_id}: native module is not published to the store")
        return 0
    pkg = deploy / "Stores" / buckets[0] / module_id / f"{module_id}.mhub"
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


def _write_from_store(deploy: Path) -> list[dict] | None:
    try:
        catalog = rebuild_catalog_from_store(deploy)
    except FileNotFoundError as exc:
        print(exc, file=sys.stderr)
        return None
    print(
        f"sync_store_catalog: wrote {deploy / 'Stores'} "
        f"({len(catalog)} modules, from store .mhub)"
    )
    return catalog


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "module_id",
        nargs="?",
        default=None,
        help="pack this id (or 'all') from sources, then rewrite catalog; "
        "omit to only scan deploy .mhub",
    )
    parser.add_argument(
        "--deploy",
        type=Path,
        default=None,
        help="deploy root (default: MINIHUB_PLUGINS_DEPLOY_DIR or ~/workspace/minihub_store)",
    )
    parser.add_argument(
        "--catalog-only",
        action="store_true",
        help="rewrite catalog.json from plugin sources (no pack)",
    )
    parser.add_argument(
        "--from-store",
        action="store_true",
        help="scan Stores/<bucket>/<id>/<id>.mhub (default when no module id is given)",
    )
    parser.add_argument(
        "--aggregate",
        type=Path,
        default=None,
        help="write the derived array to this path (e.g. a modstore seed); "
        "omitted means no aggregate is written",
    )
    args = parser.parse_args(argv)
    deploy = resolve_deploy_dir(args.deploy)
    arg = args.module_id

    def finish(catalog: list[dict] | None) -> int:
        if catalog is None:
            return 1
        if args.aggregate is not None:
            write_aggregate(args.aggregate, catalog)
            print(
                f"sync_store_catalog: aggregate → {args.aggregate} "
                f"({len(catalog)} modules)"
            )
        return 0

    if args.catalog_only:
        catalog = rebuild_catalog(deploy)
        print(
            f"sync_store_catalog: wrote {deploy / 'Stores'} "
            f"({len(catalog)} modules, catalog-only)"
        )
        return finish(catalog)

    if args.from_store or not arg:
        return finish(_write_from_store(deploy))

    if arg in ("all", "--all", "-a"):
        for module_dir in iter_module_dirs():
            rc = sync_module(module_dir.name, deploy)
            if rc != 0:
                return rc
        return finish(_write_from_store(deploy))

    rc = sync_module(arg, deploy)
    if rc != 0:
        return rc
    return finish(_write_from_store(deploy))


if __name__ == "__main__":
    raise SystemExit(main())

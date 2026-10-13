# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Shared paths for Store module sources vs deploy artifacts.

In-tree demos live under examples/plugins/<id>/. Extra / vendor trees via
MINIHUB_PLUGIN_DIRS (":" / ";" separated; each entry is either a single
plugin dir or a parent dir holding several plugins). When that env is set,
pack / catalog discovery uses only those trees (same as CMake); append
``examples/plugins`` to also pack demos. When unset, scan demos only.
Built artifacts go to:

  build/<id>.so                 — L3 MHMD+ELF intermediate (local only)
  MINIHUB_PLUGINS_DEPLOY_DIR          — default ~/workspace/minihub_store

Layout under the deploy root (one folder per L1/L2 plugin):
  Stores/Common/<id>/{catalog.json,<id>.mhub}

A native module is published under ``Stores/<chip>/<id>/`` (``<chip>`` from
``targets``); L1/L2 use ``Stores/Common/<id>/``. Firmware, asset,
and web siblings (Firmwares/, Assets/, Webs/) share this root. store_server.py
serves the whole root.
"""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEMO_PLUGINS = ROOT / "examples" / "plugins"


def default_deploy_dir() -> Path:
    env = os.environ.get("MINIHUB_PLUGINS_DEPLOY_DIR", "").strip()
    if env:
        return Path(env).expanduser().resolve()
    return (Path.home() / "workspace" / "minihub_store").resolve()


def resolve_deploy_dir(explicit: str | Path | None = None) -> Path:
    if explicit is not None and str(explicit).strip():
        return Path(explicit).expanduser().resolve()
    return default_deploy_dir()


def plugin_dir_env() -> list[Path]:
    """Split ``MINIHUB_PLUGIN_DIRS`` (``:`` or ``;``) into existing directories.

    Each entry may be a single plugin dir (containing ``module.json``) or a
    parent dir holding several plugins. Relative entries resolve against ROOT.
    """
    raw = os.environ.get("MINIHUB_PLUGIN_DIRS", "").strip()
    if not raw:
        return []
    out: list[Path] = []
    for part in raw.replace(";", ":").split(":"):
        part = part.strip()
        if not part:
            continue
        p = Path(part).expanduser()
        p = p if p.is_absolute() else (ROOT / p)
        p = p.resolve()
        if p.is_dir():
            out.append(p)
    return out


def _find_module_dir(module_id: str, *, root: Path) -> Path:
    """Locate a module source dir named ``<module_id>``.

    Search order matches CMake / Host configure:
      1. each ``MINIHUB_PLUGIN_DIRS`` entry (plugin dir itself or a parent)
      2. ``examples/plugins/<id>`` (in-tree demos; always a last-resort lookup)

    Falls back to the demo path so callers keep consistent error messages.
    """
    demo = root / "examples" / "plugins" / module_id
    for d in plugin_dir_env():
        # Entry may be the plugin dir itself — only match when the basename is
        # the requested id (avoid returning color_bulb when packing meter_plug).
        if d.name == module_id and (d / "module.json").is_file():
            return d
        cand = d / module_id
        if (cand / "module.json").is_file():
            return cand
    if (demo / "module.json").is_file():
        return demo
    return demo


def module_source_dir(module_id: str, *, root: Path = ROOT) -> Path:
    return _find_module_dir(module_id, root=root)


def iter_module_source_dirs(*, root: Path = ROOT) -> list[Path]:
    """All plugin dirs with ``module.json``.

    When ``MINIHUB_PLUGIN_DIRS`` is set, only those trees (same as CMake L3
    discovery) — Host CI that also wants demos must append
    ``examples/plugins`` to the env. When unset, scan in-tree demos.
    Dedupes by resolved path.
    """
    dirs: list[Path] = []
    seen: set[str] = set()

    def consider(d: Path) -> None:
        if not d.is_dir() or d.name.startswith(".") or d.name.startswith("_"):
            return
        if d.name == "files":
            return
        if not (d / "module.json").is_file():
            return
        key = str(d.resolve())
        if key in seen:
            return
        seen.add(key)
        dirs.append(d)

    env_dirs = plugin_dir_env()
    if env_dirs:
        for d in env_dirs:
            if (d / "module.json").is_file():
                consider(d)
            elif d.is_dir():
                for sub in sorted(d.iterdir()):
                    consider(sub)
        return dirs

    demos = root / "examples" / "plugins"
    if demos.is_dir():
        for d in sorted(demos.iterdir()):
            consider(d)
    return dirs


def store_buckets(targets: list[str]) -> list[str]:
    """Folder names under ``Stores/``.

    Any ``ALL`` (case-insensitive) selects ``Common`` only. Concrete chips
    each get their own folder, lowercased to match ``Firmwares/esp32s31``
    and the device query ``?chip=``.
    """
    chips: list[str] = []
    for raw in targets:
        if not isinstance(raw, str):
            continue
        name = raw.strip()
        if not name:
            continue
        if name.upper() == "ALL" or name == "*":
            return ["Common"]
        folder = name.lower()
        if folder not in chips:
            chips.append(folder)
    return chips or ["Common"]


def normalize_chip_folders(deploy: Path) -> None:
    """Rename chip directories to lowercase. ``Common`` is left as written.

    A case-only rename goes through a temporary name so it works on a
    case-insensitive volume.
    """
    for parent_name in ("Stores", "Firmwares"):
        parent = deploy / parent_name
        if not parent.is_dir():
            continue
        for child in list(parent.iterdir()):
            if not child.is_dir() or child.name.startswith("."):
                continue
            if child.name == "Common" or child.name == child.name.lower():
                continue
            dest = parent / child.name.lower()
            try:
                same = dest.exists() and dest.samefile(child)
            except OSError:
                same = False
            # A different directory already owns the lowercase name.
            if dest.exists() and not same:
                continue
            tmp = parent / f".{child.name.lower()}.case"
            child.rename(tmp)
            tmp.rename(dest)


def module_deploy_dirs(
    module_id: str,
    targets: list[str],
    deploy: Path | None = None,
) -> list[Path]:
    """One ``<deploy>/Stores/<bucket>/<id>`` per :func:`store_buckets` entry."""
    root = deploy or default_deploy_dir()
    return [root / "Stores" / bucket / module_id for bucket in store_buckets(targets)]


def module_deploy_dir(
    module_id: str,
    deploy: Path | None = None,
    *,
    targets: list[str] | None = None,
) -> Path:
    """Primary plugin folder. Without ``targets``, that is ``Stores/Common/<id>``."""
    if targets is None:
        root = deploy or default_deploy_dir()
        return root / "Stores" / "Common" / module_id
    return module_deploy_dirs(module_id, targets, deploy)[0]


def module_mhub_file(
    module_id: str,
    *,
    deploy: Path,
    module_dir: Path | None = None,
) -> Path:
    """``.mhub`` path for this module's ``targets`` (CMake modstore copy)."""
    from sync_store_catalog import store_buckets_for_module

    src = module_dir if module_dir is not None else module_source_dir(module_id)
    manifest: dict = {}
    manifest_path = src / "module.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    mid = manifest.get("id") if isinstance(manifest.get("id"), str) else None
    mid = mid or module_id
    buckets = store_buckets_for_module(manifest, src if manifest_path.is_file() else module_dir or src)
    if not buckets:
        raise FileNotFoundError(f"{mid} is not published to the store")
    return deploy / "Stores" / buckets[0] / mid / f"{mid}.mhub"


def iter_store_plugin_dirs(deploy: Path) -> list[Path]:
    """``Stores/<bucket>/<id>/`` directories. Skips the derived catalog file."""
    stores = deploy / "Stores"
    if not stores.is_dir():
        return []
    out: list[Path] = []
    buckets = sorted(
        p for p in stores.iterdir() if p.is_dir() and not p.name.startswith(".")
    )
    for bucket in buckets:
        for child in sorted(bucket.iterdir()):
            if child.is_dir() and not child.name.startswith("."):
                out.append(child)
    return out


def find_deploy_mhubs(module_id: str, deploy: Path) -> list[Path]:
    out: list[Path] = []
    for child in iter_store_plugin_dirs(deploy):
        if child.name != module_id:
            continue
        pkg = child / f"{module_id}.mhub"
        if pkg.is_file():
            out.append(pkg)
    return out


def catalog_preference(entry: dict) -> tuple[int, str]:
    """Sort key: the catalog object whose ``url`` matches the primary bucket."""
    raw = entry.get("targets")
    names = [item for item in raw if isinstance(item, str)] if isinstance(raw, list) else []
    primary = store_buckets(names)[0]
    url = entry.get("url") if isinstance(entry.get("url"), str) else ""
    hit = 0 if f"/Stores/{primary}/" in url else 1
    return (hit, url)


def prune_plugin_dirs(deploy: Path, module_id: str, keep_buckets: list[str]) -> None:
    """Remove ``Stores/<other>/<id>/`` left behind by an older bucket choice."""
    stores = deploy / "Stores"
    if not stores.is_dir() or not module_id or module_id in (".", ".."):
        return
    keep = {name.casefold() for name in keep_buckets}
    for bucket in stores.iterdir():
        if not bucket.is_dir() or bucket.name.startswith(".") or bucket.name.casefold() in keep:
            continue
        stale = bucket / module_id
        if stale.is_dir():
            shutil.rmtree(stale)
        if bucket.name != "Common":
            try:
                next(bucket.iterdir())
            except StopIteration:
                bucket.rmdir()


def module_mhmd_path(module_id: str, *, root: Path = ROOT) -> Path:
    """MHMD .so for pack: Host build first, else standalone plugin/build/<id>.so."""
    app = os.environ.get("MINIHUB_APP", "minihub").strip() or "minihub"
    host_so = root / "application" / app / "build" / f"{module_id}.so"
    if host_so.is_file():
        return host_so

    candidates: list[Path] = [
        root / "examples" / "plugins" / module_id / "build" / f"{module_id}.so",
    ]
    for d in plugin_dir_env():
        if d.name == module_id:
            candidates.append(d / "build" / f"{module_id}.so")
        elif (d / module_id / "module.json").is_file():
            candidates.append(d / module_id / "build" / f"{module_id}.so")
    for cand in candidates:
        if cand.is_file():
            return cand
    return host_so

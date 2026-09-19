# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Shared paths for Store module sources vs deploy artifacts.

Sources stay under components/minihub_plugins/<id>/ (module.json, sources, ui).
If a module is not under that built-in dir, it can be located via the
MINIHUB_PLUGIN_DIRS env var (":" / ";" separated; each entry is either a
single plugin dir or a parent dir holding several plugins).
Built artifacts go to:

  build/<id>.so                 — L3 MHMD+ELF intermediate (local only)
  MINIHUB_STORE_DEPLOY          — default ~/workspace/minihub_store

Layout under deploy (Store-facing only):
  catalog.json
  <id>/<id>.mhub   (zip upgrade package; may contain <id>.so inside the zip)
"""

from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORE_MODULES = ROOT / "components" / "minihub_plugins"


def default_deploy_dir() -> Path:
    env = os.environ.get("MINIHUB_STORE_DEPLOY", "").strip()
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

    Search order: built-in ``components/minihub_plugins/<id>``, then each
    ``MINIHUB_PLUGIN_DIRS`` entry (which may be the plugin dir itself or a
    parent dir). Falls back to the built-in path so callers keep consistent
    error messages.
    """
    builtin = root / "components" / "minihub_plugins" / module_id
    if (builtin / "module.json").is_file():
        return builtin
    for d in plugin_dir_env():
        # Entry may be the plugin dir itself — only match when the basename is
        # the requested id (avoid returning color_bulb when packing meter_plug).
        if d.name == module_id and (d / "module.json").is_file():
            return d
        cand = d / module_id
        if (cand / "module.json").is_file():
            return cand
    return builtin


def module_source_dir(module_id: str, *, root: Path = ROOT) -> Path:
    return _find_module_dir(module_id, root=root)


def iter_module_source_dirs(*, root: Path = ROOT) -> list[Path]:
    """All plugin dirs with ``module.json``: ``minihub_plugins`` + ``MINIHUB_PLUGIN_DIRS``.

    Dedupes by resolved path so the same plugin is not listed twice when it is
    reachable via both the built-in tree and an env entry.
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

    store = root / "components" / "minihub_plugins"
    if store.is_dir():
        for d in sorted(store.iterdir()):
            consider(d)
    for d in plugin_dir_env():
        if (d / "module.json").is_file():
            consider(d)
        elif d.is_dir():
            for sub in sorted(d.iterdir()):
                consider(sub)
    return dirs


def module_deploy_dir(module_id: str, deploy: Path | None = None) -> Path:
    return (deploy or default_deploy_dir()) / module_id


def module_mhmd_path(module_id: str, *, root: Path = ROOT) -> Path:
    """Local MHMD intermediate: application/<app>/build/<id>.so (not under deploy)."""
    app = os.environ.get("MINIHUB_APP", "minihub").strip() or "minihub"
    return root / "application" / app / "build" / f"{module_id}.so"

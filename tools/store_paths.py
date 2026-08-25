# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Shared paths for Store module sources vs deploy artifacts.

Sources stay under components/store_modules/<id>/ (module.json, sources, ui).
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
STORE_MODULES = ROOT / "components" / "store_modules"


def default_deploy_dir() -> Path:
    env = os.environ.get("MINIHUB_STORE_DEPLOY", "").strip()
    if env:
        return Path(env).expanduser().resolve()
    return (Path.home() / "workspace" / "minihub_store").resolve()


def resolve_deploy_dir(explicit: str | Path | None = None) -> Path:
    if explicit is not None and str(explicit).strip():
        return Path(explicit).expanduser().resolve()
    return default_deploy_dir()


def module_source_dir(module_id: str, *, root: Path = ROOT) -> Path:
    return root / "components" / "store_modules" / module_id


def module_deploy_dir(module_id: str, deploy: Path | None = None) -> Path:
    return (deploy or default_deploy_dir()) / module_id


def module_mhmd_path(module_id: str, *, root: Path = ROOT) -> Path:
    """Local MHMD intermediate: application/<app>/build/<id>.so (not under deploy)."""
    app = os.environ.get("MINIHUB_APP", "minihub").strip() or "minihub"
    return root / "application" / app / "build" / f"{module_id}.so"

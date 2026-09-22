# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage downloadable HTML extras: type catalog and L3 module SDK zip.

Sphinx ``html_static_path`` ships ``_static/catalog/`` and
``_static/module_sdk.zip``. Do not edit staged copies.
"""

from __future__ import annotations

import shutil
import zipfile
from pathlib import Path

_DOCS_DIR = Path(__file__).resolve().parents[1]
_REPO = _DOCS_DIR.parent
_WIDGET = _REPO / "components" / "minihub_device_widget"
_STAGED = _DOCS_DIR / "_static" / "catalog"


def _copy_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def stage_catalog_assets() -> Path:
    """Refresh ``docs/_static/catalog/`` from the widget component + demos."""
    if _STAGED.exists():
        shutil.rmtree(_STAGED)
    model_dir = _WIDGET / "model"
    if not model_dir.is_dir():
        raise FileNotFoundError(f"type catalog missing: {model_dir}")

    for csv in sorted(model_dir.glob("*.csv")):
        _copy_file(csv, _STAGED / "model" / csv.name)

    images = _WIDGET / "images" / "devices"
    if images.is_dir():
        for svg in sorted(images.glob("*.svg")):
            _copy_file(svg, _STAGED / "images" / "devices" / svg.name)

    demos = _REPO / "examples" / "plugins"
    for demo in ("color_bulb", "meter_plug"):
        dj = demos / demo / "devices.json"
        if dj.is_file():
            _copy_file(dj, _STAGED / "examples" / demo / "devices.json")

    for name in ("STANDARD_TYPES.md", "STANDARD_TYPES_CN.md"):
        src = _WIDGET / name
        if src.is_file():
            _copy_file(src, _STAGED / name)

    zip_path = _DOCS_DIR / "_static" / "catalog.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(_STAGED.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(_STAGED).as_posix())
    return _STAGED


_SDK_SRC = _DOCS_DIR / "module_sdk"
_SDK_ZIP = _DOCS_DIR / "_static" / "module_sdk.zip"


def stage_module_sdk_zip() -> Path:
    """Pack Plugin SDK headers, cmake, and pack scripts (same layout as docs/module_sdk/)."""
    include = _SDK_SRC / "include"
    if not include.is_dir():
        raise FileNotFoundError(f"module SDK missing: {include}")

    _SDK_ZIP.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(_SDK_ZIP, "w", zipfile.ZIP_DEFLATED) as zf:
        extra = (
            _SDK_SRC / "README.md",
            _SDK_SRC / "host_export_allowlist.json",
            _SDK_SRC / "minihub_plugin_standalone.cmake",
            _SDK_SRC / "pack_store_module.py",
            _SDK_SRC / "store_paths.py",
            _SDK_SRC / "sync_store_catalog.py",
            _SDK_SRC / "build_plugin_standalone.py",
        )
        for src in extra:
            real = src.resolve()
            if not real.is_file():
                raise FileNotFoundError(f"module SDK zip missing {src}")
            # Git tree may symlink pack_*.py → tools/; zip always stores the file body.
            zf.write(real, f"module_sdk/{src.name}")
        for path in sorted(_SDK_SRC.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(_SDK_SRC).as_posix()
            if rel.startswith("include/") or rel.startswith("templates/"):
                zf.write(path.resolve(), f"module_sdk/{rel}")
    return _SDK_ZIP

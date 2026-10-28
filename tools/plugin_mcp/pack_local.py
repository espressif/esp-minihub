# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Pack a plugin directory from anywhere, reusing the canonical packer's rules.

Classification, the packaged manifest, meta.json and the MHMD image header all
come from ``tools/pack_store_module.py`` — the same code the firmware build uses,
so what validate/pack says here matches what the Hub accepts.
"""

from __future__ import annotations

import json
import struct
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any

from .paths import TOOLS_DIR
from .validate import validate_plugin_dir

if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from pack_store_module import (  # noqa: E402  (needs TOOLS_DIR on sys.path)
    MHMD_MAGIC,
    build_package_meta,
    is_config_only,
    is_hybrid_module,
    is_js_module,
    packaged_manifest,
    wrap_mhmd,
)


def _has_mhmd_header(path: Path) -> bool:
    try:
        with path.open("rb") as fh:
            head = fh.read(4)
    except OSError:
        return False
    return len(head) == 4 and struct.unpack("<I", head)[0] == MHMD_MAGIC


def pack_plugin_dir(module_dir: Path, out_path: Path | None = None) -> dict[str, Any]:
    report = validate_plugin_dir(module_dir)
    if not report["ok"]:
        return {"ok": False, "pack": None, "validate": report}

    module_dir = Path(report["dir"])
    manifest = json.loads((module_dir / "module.json").read_text(encoding="utf-8"))
    module_id = str(manifest.get("id") or module_dir.name)
    pkg = out_path.expanduser().resolve() if out_path else (module_dir / f"{module_id}.mhub")
    pkg.parent.mkdir(parents=True, exist_ok=True)

    config_only = is_config_only(manifest)
    hybrid = is_hybrid_module(manifest)
    js = is_js_module(manifest, module_dir)

    names: list[str] = []
    # Native/hybrid need the MHMD-wrapped image the dynamic loader validates; a
    # bare linker output is rejected on device with ESP_ERR_INVALID_CRC.
    so: Path | None = None
    tmp: tempfile.TemporaryDirectory[str] | None = None
    if not config_only and (hybrid or not js):
        so_name = str(manifest.get("binary") or f"{module_id}.so")
        found = module_dir / so_name
        if not found.is_file():
            return {
                "ok": False,
                "pack": None,
                "validate": report,
                "error": (
                    f"native/hybrid pack needs {found}. Build it on Host (IDF 6.2 / "
                    "esp32s31); this MCP does not compile .so."
                ),
            }
        if _has_mhmd_header(found):
            so = found
        else:
            tmp = tempfile.TemporaryDirectory(prefix="minihub-mhmd-")
            so = Path(tmp.name) / f"{module_id}.so"
            wrap_mhmd(found, so)

    try:
        pkg_manifest = packaged_manifest(
            manifest, module_dir=module_dir, module_id=module_id, so=so
        )
        meta = build_package_meta(module_dir, manifest)

        with zipfile.ZipFile(pkg, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(
                "module.json",
                json.dumps(pkg_manifest, indent=2, ensure_ascii=False) + "\n",
            )
            names.append("module.json")
            if meta:
                zf.writestr(
                    "meta.json", json.dumps(meta, indent=2, ensure_ascii=False) + "\n"
                )
                names.append("meta.json")

            if not config_only and js:
                main_js = module_dir / "main.js"
                if not main_js.is_file():
                    return {
                        "ok": False,
                        "pack": None,
                        "validate": report,
                        "error": f"L2 missing {main_js}",
                    }
                zf.write(main_js, "main.js")
                names.append("main.js")
            elif so is not None:
                zf.write(so, f"{module_id}.so")
                names.append(f"{module_id}.so")
                if hybrid:
                    main_js = module_dir / "main.js"
                    if not main_js.is_file():
                        return {
                            "ok": False,
                            "pack": None,
                            "validate": report,
                            "error": f"hybrid missing {main_js}",
                        }
                    zf.write(main_js, "main.js")
                    names.append("main.js")

            if not config_only:
                for extra in ("devices.json", "catalog.json"):
                    path = module_dir / extra
                    if path.is_file():
                        zf.write(path, extra)
                        names.append(extra)
                web = module_dir / "web"
                if web.is_dir():
                    for path in sorted(web.rglob("*")):
                        if path.is_file():
                            arc = path.relative_to(module_dir).as_posix()
                            zf.write(path, arc)
                            names.append(arc)
    finally:
        if tmp is not None:
            tmp.cleanup()

    return {
        "ok": True,
        "pack": str(pkg),
        "size": pkg.stat().st_size,
        "entries": names,
        "validate": report,
        "note": (
            "Install via Store catalog (custom_url) + refresh + install. "
            "Outer format is zip .mhub; ui and capabilities travel in meta.json."
        ),
    }

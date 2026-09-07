# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Pack L1/L2 (and hybrid/native if .so is already in the module dir) from any folder."""

from __future__ import annotations

import json
import zipfile
from pathlib import Path
from typing import Any

from .validate import validate_plugin_dir


def _is_config_only(manifest: dict[str, Any]) -> bool:
    if manifest.get("config_only") is True:
        return True
    if manifest.get("runtime") == "pack":
        return True
    if manifest.get("binary") or manifest.get("runtime") in ("js", "native", "hybrid"):
        return False
    if manifest.get("inject") or manifest.get("requires"):
        return True
    return False


def _is_js(manifest: dict[str, Any], module_dir: Path) -> bool:
    if manifest.get("runtime") == "hybrid":
        return False
    if manifest.get("runtime") == "js":
        return True
    if manifest.get("runtime") == "native":
        return False
    if (module_dir / "CMakeLists.txt").is_file():
        return False
    return (module_dir / "main.js").is_file()


def pack_plugin_dir(module_dir: Path, out_path: Path | None = None) -> dict[str, Any]:
    report = validate_plugin_dir(module_dir)
    if not report["ok"]:
        return {"ok": False, "pack": None, "validate": report}

    module_dir = Path(report["dir"])
    manifest = json.loads((module_dir / "module.json").read_text(encoding="utf-8"))
    module_id = str(manifest.get("id") or module_dir.name)
    pkg = out_path.expanduser().resolve() if out_path else (module_dir / f"{module_id}.mhub")
    pkg.parent.mkdir(parents=True, exist_ok=True)

    names: list[str] = []
    with zipfile.ZipFile(pkg, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("module.json", json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
        names.append("module.json")

        if _is_config_only(manifest):
            pass
        elif _is_js(manifest, module_dir):
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
        else:
            so_name = str(manifest.get("binary") or f"{module_id}.so")
            so = module_dir / so_name
            if not so.is_file():
                return {
                    "ok": False,
                    "pack": None,
                    "validate": report,
                    "error": (
                        f"native/hybrid pack needs {so}. Build on Host (IDF 6.2 / esp32s31); "
                        "this MCP does not compile .so."
                    ),
                }
            zf.write(so, f"{module_id}.so")
            names.append(f"{module_id}.so")
            if manifest.get("runtime") == "hybrid":
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

        if not _is_config_only(manifest):
            ui = module_dir / "ui.json"
            if ui.is_file():
                zf.write(ui, "ui.json")
                names.append("ui.json")
            web = module_dir / "web"
            if web.is_dir():
                for path in sorted(web.rglob("*")):
                    if path.is_file():
                        arc = path.relative_to(module_dir).as_posix()
                        zf.write(path, arc)
                        names.append(arc)

    return {
        "ok": True,
        "pack": str(pkg),
        "size": pkg.stat().st_size,
        "entries": names,
        "validate": report,
        "note": "Install via Store catalog (custom_url) + refresh + install. Outer format is zip .mhub.",
    }

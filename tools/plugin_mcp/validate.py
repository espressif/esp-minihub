# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from .ids import INJECT_ADAPTERS, JS_FORBIDDEN, RUNTIME_VALUES, TYPE_VALUES, check_module_id


def _issue(path: str, message: str, *, level: str = "error") -> dict[str, str]:
    return {"level": level, "path": path, "message": message}


def validate_module_json(manifest: dict[str, Any], *, source: str = "module.json") -> list[dict[str, str]]:
    issues: list[dict[str, str]] = []
    mid = str(manifest.get("id") or "")
    err = check_module_id(mid) if mid else "missing id"
    if err:
        issues.append(_issue(source, err))

    kind = manifest.get("type")
    if kind is not None and kind not in TYPE_VALUES:
        issues.append(_issue(source, f"type must be {TYPE_VALUES}, got {kind!r}"))

    runtime = manifest.get("runtime")
    if runtime is not None and runtime not in RUNTIME_VALUES:
        issues.append(_issue(source, f"runtime must be {RUNTIME_VALUES}, got {runtime!r}"))

    inject = manifest.get("inject")
    if isinstance(inject, dict):
        adapter = inject.get("adapter")
        if not adapter:
            issues.append(_issue(source, "inject.adapter is required"))
        elif adapter not in INJECT_ADAPTERS:
            issues.append(
                _issue(
                    source,
                    f"inject.adapter {adapter!r} is not a Host adapter "
                    f"({', '.join(INJECT_ADAPTERS)}). L1 cannot add a new radio stack.",
                )
            )

    if runtime == "js" and kind == "pack":
        issues.append(_issue(source, 'L2 JS should use type "module", not "pack"'))
    if runtime == "pack" and kind == "module":
        issues.append(_issue(source, 'L1 pack should use type "pack"', level="warning"))

    return issues


def validate_js_source(text: str, *, path: str = "main.js") -> list[dict[str, str]]:
    issues: list[dict[str, str]] = []
    for token in JS_FORBIDDEN:
        if token in text:
            issues.append(_issue(path, f"forbidden native API token {token!r} in L2 JS"))
    if re.search(r"host\.command\.invoke\s*\([^)]*params", text):
        issues.append(_issue(path, "host.command.invoke has no params field today; use { point_id, cmd }"))
    return issues


def validate_plugin_dir(module_dir: Path | str) -> dict[str, Any]:
    module_dir = Path(module_dir).expanduser().resolve()
    manifest_path = module_dir / "module.json"
    issues: list[dict[str, str]] = []
    if not manifest_path.is_file():
        return {
            "ok": False,
            "dir": str(module_dir),
            "issues": [_issue("module.json", f"missing {manifest_path}")],
        }

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return {
            "ok": False,
            "dir": str(module_dir),
            "issues": [_issue("module.json", f"invalid JSON: {exc}")],
        }

    if not isinstance(manifest, dict):
        return {
            "ok": False,
            "dir": str(module_dir),
            "issues": [_issue("module.json", "root must be an object")],
        }

    issues.extend(validate_module_json(manifest))
    mid = str(manifest.get("id") or "")
    if mid and module_dir.name != mid:
        issues.append(
            _issue(
                "module.json",
                f"id {mid!r} must equal directory name {module_dir.name!r}",
            )
        )

    runtime = manifest.get("runtime")
    main_js = module_dir / "main.js"
    cmake = module_dir / "CMakeLists.txt"

    if runtime == "js" or (main_js.is_file() and runtime not in ("native", "hybrid", "pack")):
        if not main_js.is_file():
            issues.append(_issue("main.js", "L2 module missing main.js"))
        else:
            issues.extend(validate_js_source(main_js.read_text(encoding="utf-8")))

    if runtime == "pack" or manifest.get("config_only") is True:
        if main_js.is_file():
            issues.append(_issue("main.js", "L1 pack should not include main.js", level="warning"))
        if cmake.is_file():
            issues.append(_issue("CMakeLists.txt", "L1 pack should not include CMakeLists.txt"))

    if runtime == "native":
        if not cmake.is_file():
            issues.append(_issue("CMakeLists.txt", "L3 native module usually has CMakeLists.txt", level="warning"))

    errors = [i for i in issues if i["level"] == "error"]
    return {
        "ok": not errors,
        "dir": str(module_dir),
        "id": mid,
        "issues": issues,
    }

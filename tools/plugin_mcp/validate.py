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

    # minihub_module's validate_module_info() rejects a manifest with an empty id
    # OR an empty adapter name, for every tier that reaches the module table.
    # minihub_js only starts QuickJS for runtime exactly "js"/"hybrid", so
    # omitting runtime on a JS module does not fall back to JS on device.
    if kind != "pack" and runtime != "pack" and manifest.get("config_only") is not True:
        if not manifest.get("adapter"):
            issues.append(
                _issue(
                    source,
                    "adapter is required: it names the adapters.json slot and the "
                    "loader rejects a manifest without one (also for L2 JS)",
                )
            )
        if runtime is None and manifest.get("binary") is None and manifest.get("builtin") is None:
            issues.append(
                _issue(
                    source,
                    'runtime is required: write "js" for a main.js module — the '
                    "device does not infer JS from the presence of main.js",
                )
            )

    inject = manifest.get("inject")
    if isinstance(inject, dict):
        adapter = inject.get("adapter")
        if not adapter:
            issues.append(_issue(source, "inject.adapter is required"))
        elif adapter not in INJECT_ADAPTERS:
            issues.append(
                _issue(
                    source,
                    f"inject.adapter {adapter!r} is not one of the bundled adapters "
                    f"({', '.join(INJECT_ADAPTERS)}). The merge does create the key and "
                    "enables it, but with no adapter implementation behind that name "
                    "the block is inert — only an L2/L3 module can supply one.",
                    level="warning",
                )
            )

    if runtime == "js" and kind == "pack":
        issues.append(_issue(source, 'L2 JS should use type "module", not "pack"'))
    if runtime == "pack" and kind == "module":
        issues.append(_issue(source, 'L1 pack should use type "pack"', level="warning"))

    return issues


def _strip_js_noise(text: str) -> str:
    """Drop comments and string literals so the native-API scan sees real calls only."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', text)
    return text


def validate_js_source(text: str, *, path: str = "main.js") -> list[dict[str, str]]:
    issues: list[dict[str, str]] = []
    scan = _strip_js_noise(text)
    for token in JS_FORBIDDEN:
        if token in scan:
            issues.append(_issue(path, f"forbidden native API token {token!r} in L2 JS"))
    # The runtime evaluates main.js as a script and only then looks up a global
    # setup; there is no other entry point, so a file without it loads and does
    # nothing (Host logs "no setup(host) function").
    if not re.search(r"\bfunction\s+setup\s*\(\s*host\s*\)", scan) \
            and not re.search(r"\bsetup\s*=\s*(?:function|\()", scan):
        issues.append(
            _issue(
                path,
                "no global setup(host) — Host calls setup() after evaluating main.js "
                "and skips the module silently when it is missing",
            )
        )
    # host is passed as setup's argument, not installed as a global: touching it at
    # file level throws ReferenceError while main.js is evaluated, before setup is
    # ever looked up.
    setup_at = scan.find("function setup")
    used_at = scan.find("host.")
    if used_at != -1 and (setup_at == -1 or used_at < setup_at):
        issues.append(
            _issue(
                path,
                "host is only available inside setup(host) — using it at file level "
                "throws ReferenceError while main.js is evaluated",
            )
        )
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

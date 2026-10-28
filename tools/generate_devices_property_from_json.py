#!/usr/bin/env python3
"""Generate a device property header (device_desc.gen.h) from a plugin's devices.json.

Shared MiniHub helper, run MANUALLY before building a plugin (NOT wired into
CMake). It embeds the device descriptor as a C string so the plugin is
self-describing at runtime with no filesystem dependency: the simulated device
(params, initial values, ranges and display format) is defined entirely in the
plugin's devices.json and compiled into the module image as DEVICE_DESC_JSON.

The JSON is validated (json.loads) and minified before embedding so trailing
commas / comments cannot sneak into DEVICE_DESC_JSON and break cJSON_Parse on
device.

Per-device and per-parameter overrides are validated here so a bad descriptor
fails at generation time instead of rendering silently wrong on the Hub:

* ``icon``    must reference an icon that exists under
              ``components/minihub_device_widget/images`` (``./images/...``).
              Plugins cannot ship their own artwork, so anything unknown is an
              error, not a broken image later.
* ``ui_element`` must come from the closed widget set
              (ui.toggle / ui.slider / ui.dropdown / ui.text / ui.hidden /
              ui.button).
* ``css``     must be plain class names (``[A-Za-z0-9_-]`` plus spaces). Anything
              else could smuggle markup into the page.

Leaving them out keeps the catalog default for the device ``type``.

Usage:
    generate_devices_property_from_json.py <devices.json> <out.h>
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE_DIR = ROOT / "components" / "minihub_device_widget" / "images"

CLOSED_UI = {"ui.toggle", "ui.slider", "ui.dropdown", "ui.text", "ui.hidden",
             "ui.button"}
CSS_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*(?: [A-Za-z0-9][A-Za-z0-9_-]*)*$")

# Everything a plugin may set on a parameter entry.  Anything else is a typo
# (or an unsupported field) and must fail here rather than being dropped
# silently by the adapter later.
PARAM_KEYS = ("name", "init",
              "visible", "access", "min", "max", "step", "unit", "requires",
              "icon", "ui_element", "css")


def known_icons():
    """Every icon a plugin is allowed to point at, as './images/<rel path>'."""
    out = set()
    if IMAGE_DIR.is_dir():
        for p in IMAGE_DIR.rglob("*.svg"):
            out.add("./images/" + p.relative_to(IMAGE_DIR).as_posix())
    return out


def check_entry(obj, where, icons, errors):
    if not isinstance(obj, dict):
        errors.append(f"{where}: expected an object")
        return
    icon = obj.get("icon")
    if icon is not None:
        if not isinstance(icon, str) or icon not in icons:
            errors.append(
                f"{where}: icon '{icon}' is not a shipped icon "
                f"(must be one of {sorted(icons) or '[]'}); omit it to use the "
                f"type default")
    ui = obj.get("ui_element")
    if ui is not None:
        if not isinstance(ui, str) or ui not in CLOSED_UI:
            errors.append(
                f"{where}: ui_element '{ui}' is outside the closed widget set "
                f"{sorted(CLOSED_UI)}")
    css = obj.get("css")
    if css is not None:
        if not isinstance(css, str) or not CSS_RE.match(css):
            errors.append(
                f"{where}: css '{css}' must be plain class names "
                f"(letters, digits, '-', '_', space-separated)")


def check(data, icons):
    errors = []
    if isinstance(data, dict):
        data = [data]
    if not isinstance(data, list):
        return ["top level: expected an object or array of device objects"]
    for i, dev in enumerate(data):
        where = f"device[{i}]"
        if not isinstance(dev, dict):
            errors.append(f"{where}: expected an object")
            continue
        name = dev.get("device") or f"#{i}"
        where = f"device '{name}'"
        check_entry(dev, where, icons, errors)
        params = dev.get("params")
        if params is None:
            errors.append(f"{where}: missing 'params'")
            continue
        if not isinstance(params, list):
            errors.append(f"{where}: 'params' must be an array")
            continue
        for j, p in enumerate(params):
            if not isinstance(p, dict):
                errors.append(f"{where}: params[{j}] is not an object")
                continue
            pname = p.get("name") or f"#{j}"
            if not isinstance(pname, str) or not pname:
                errors.append(f"{where}: params[{j}] is missing 'name'")
                continue
            check_entry(p, f"{where}: param '{pname}'", icons, errors)
            for k in p:
                if k not in PARAM_KEYS:
                    errors.append(
                        f"{where}: param '{pname}' has unknown key '{k}' "
                        f"(allowed: {', '.join(PARAM_KEYS)})")
    return errors


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: generate_devices_property_from_json.py <devices.json> <out.h>\n")
        return 2
    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, "r", encoding="utf-8") as f:
        raw = f.read()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        sys.stderr.write(f"{src_path}: invalid JSON: {e}\n")
        return 1

    errors = check(data, known_icons())
    if errors:
        sys.stderr.write(f"{src_path}: rejected ({len(errors)} problem(s)):\n")
        for e in errors:
            sys.stderr.write(f"  - {e}\n")
        return 1

    # Compact form: no trailing commas, no pretty whitespace, ASCII-safe.
    src = json.dumps(data, ensure_ascii=True, separators=(",", ":"))
    # Escape for a C string literal: backslash, then double-quote.
    # Minified JSON has no newlines; strip any that appear anyway.
    escaped = (
        src.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "")
    )
    header = (
        "/* Generated from devices.json - DO NOT EDIT */\n"
        "#pragma once\n"
        "static const char DEVICE_DESC_JSON[] =\n"
        '"' + escaped + '";\n'
    )
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

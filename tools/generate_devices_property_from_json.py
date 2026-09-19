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

Usage:
    generate_devices_property_from_json.py <devices.json> <out.h>
"""
import json
import sys


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

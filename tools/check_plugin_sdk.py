#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Validate docs/module_sdk against Host IDF / MiniHub declarations.

Modes:
  coverage   — every allowlisted non-libc symbol is declared in minihub_plugin_sdk.h
  emit-sizes — cross-compile SDK-only probe; write sizeof macros for Host _Static_assert
  check      — coverage + emit-sizes + (optional) Host include-dir proto compile

Host firmware CMake runs ``check`` / ``emit-sizes`` when CONFIG_MINIHUB_DYNAMIC_MODULES.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SDK = ROOT / "docs" / "module_sdk" / "include" / "minihub_plugin_sdk.h"
DEFAULT_SDK_DIR = ROOT / "docs" / "module_sdk" / "include" / "minihub_plugin_sdk"
DEFAULT_ALLOWLIST = ROOT / "docs" / "module_sdk" / "host_export_allowlist.json"

# Toolchain libc / libgcc — declared by the cross compiler, not the SDK surface.
LIBC_OR_LIBGCC = frozenset(
    {
        "atof",
        "atoi",
        "calloc",
        "close",
        "fclose",
        "fopen",
        "fread",
        "free",
        "fseek",
        "ftell",
        "fwrite",
        "malloc",
        "memcmp",
        "memcpy",
        "mkdir",
        "memmove",
        "memset",
        "realloc",
        "rewind",
        "rmdir",
        "snprintf",
        "sscanf",
        "stat",
        "strcasecmp",
        "strcat",
        "strchr",
        "strcmp",
        "strcpy",
        "strdup",
        "strlen",
        "strncat",
        "strncmp",
        "strncpy",
        "strrchr",
        "strstr",
        "strtol",
        "time",
        "unlink",
        "vsnprintf",
        "__divdi3",
        "__eqdf2",
        "__extendsfdf2",
        "__fixdfdi",
        "__fixdfsi",
        "__floatdidf",
        "__floatsidf",
        "__floatunsidf",
        "__gedf2",
        "__gtdf2",
        "__ledf2",
        "__ltdf2",
        "__nedf2",
    }
)

# Stack / opaque structs whose layout must match Host (SDK vs IDF/MiniHub).
LAYOUT_TYPES = (
    "esp_http_client_config_t",
    "esp_http_client_event_t",
    "esp_log_config_t",
    "cJSON",
    "mbedtls_md_context_t",
    "psa_key_attributes_t",
    "minihub_point_t",
    "minihub_event_t",
    "minihub_command_t",
    "minihub_adapter_t",
    "minihub_module_host_api_t",
    "minihub_ble_gatt_adv_t",
)


def load_allowlist(path: Path) -> list[str]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    return list(doc.get("symbols") or [])


def sdk_declares_symbol(sdk_text: str, sym: str) -> bool:
    # Function or object declaration mentioning the symbol as an identifier.
    return re.search(rf"(?<![\w]){re.escape(sym)}\s*\(", sdk_text) is not None or re.search(
        rf"(?<![\w]){re.escape(sym)}\s*;", sdk_text
    ) is not None


def load_sdk_text(sdk: Path) -> str:
    """Umbrella + category headers under minihub_plugin_sdk/."""
    parts = [sdk.read_text(encoding="utf-8")]
    cat = sdk.parent / "minihub_plugin_sdk"
    if cat.is_dir():
        for p in sorted(cat.glob("*.h")):
            parts.append(p.read_text(encoding="utf-8"))
    return "\n".join(parts)


def check_coverage(sdk: Path, allowlist: Path) -> int:
    text = load_sdk_text(sdk)
    missing = []
    for sym in load_allowlist(allowlist):
        if sym in LIBC_OR_LIBGCC:
            continue
        if not sdk_declares_symbol(text, sym):
            missing.append(sym)
    if missing:
        print("check_plugin_sdk: missing declarations in", sdk, file=sys.stderr)
        for s in missing:
            print(f"  - {s}", file=sys.stderr)
        return 1
    print(f"check_plugin_sdk: coverage ok ({sdk.name} + minihub_plugin_sdk/*.h)")
    return 0


def _tool_from_cc(cc: str, name: str) -> str:
    base = Path(cc)
    candidate = base.with_name(base.name.replace("gcc", name).replace("clang", name))
    if candidate.exists():
        return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    # Espressif: riscv32-esp-elf-nm
    prefix = re.sub(r"-gcc$", "", base.name)
    alt = base.with_name(f"{prefix}-{name}")
    if alt.exists():
        return str(alt)
    which_alt = shutil.which(f"{prefix}-{name}")
    return which_alt or name


def emit_sizes(cc: str, sdk_inc: Path, out: Path, cflags: tuple[str, ...] = ()) -> int:
    """Compile SDK-only sizeof probe to asm; emit #define MH_SDK_SIZE_<type> <n>.

    When invoked from a Host firmware build, `cflags` carries
    -DMH_PLUGIN_SDK_HOST_BUILD plus the build's sdkconfig.h include, so the
    mirrored structs are derived from the *actual* Host sdkconfig rather than the
    baked-in canonical. This makes MH_SDK_SIZE match the Host build for any
    target/config and still verifies the mirror reproduces the real layout.
    """
    lines = "\n".join(f"char mh_sdk_sz_{t}[sizeof({t})];" for t in LAYOUT_TYPES)
    src = '#include "minihub_plugin_sdk.h"\n' + lines + "\n"
    with tempfile.TemporaryDirectory(prefix="mh_sdk_sz_") as td:
        td_path = Path(td)
        c_path = td_path / "probe.c"
        s_path = td_path / "probe.s"
        c_path.write_text(src, encoding="utf-8")
        cmd = [
            cc,
            "-std=gnu17",
            "-S",
            "-fno-common",
            *cflags,
            "-o",
            str(s_path),
            f"-I{sdk_inc}",
            str(c_path),
        ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("check_plugin_sdk: emit-sizes compile failed:", file=sys.stderr)
            print(r.stderr, file=sys.stderr)
            return 1
        asm = s_path.read_text(encoding="utf-8", errors="replace")
        sizes: dict[str, int] = {}
        # GNU: .size mh_sdk_sz_TYPE, 152   or  .comm mh_sdk_sz_TYPE,152,4
        # Apple: .comm _mh_sdk_sz_TYPE,152
        for t in LAYOUT_TYPES:
            patterns = [
                rf"\.size\s+_?mh_sdk_sz_{re.escape(t)}\s*,\s*(\d+)",
                rf"\.comm\s+_?mh_sdk_sz_{re.escape(t)}\s*,\s*(\d+)",
                rf"\.zerofill\s+\S+,_?mh_sdk_sz_{re.escape(t)},(\d+)",
                rf"\.space\s+(\d+).*mh_sdk_sz_{re.escape(t)}",
            ]
            found = None
            for pat in patterns:
                m = re.search(pat, asm)
                if m:
                    found = int(m.group(1))
                    break
            if found is None:
                # Fallback: zero-init array as .byte/.zero N
                m = re.search(
                    rf"_?mh_sdk_sz_{re.escape(t)}:.*?\.zero\s+(\d+)",
                    asm,
                    flags=re.S,
                )
                if m:
                    found = int(m.group(1))
            if found is None:
                print(f"check_plugin_sdk: cannot parse sizeof({t}) from asm", file=sys.stderr)
                print(asm[:2000], file=sys.stderr)
                return 1
            sizes[t] = found
        out.parent.mkdir(parents=True, exist_ok=True)
        body = [
            "/* Auto-generated by tools/check_plugin_sdk.py emit-sizes — do not edit */",
            "#pragma once",
            "",
        ]
        for t in LAYOUT_TYPES:
            body.append(f"#define MH_SDK_SIZE_{t} {sizes[t]}u")
        body.append("")
        out.write_text("\n".join(body), encoding="utf-8")
        print(f"check_plugin_sdk: wrote {out}")
        for t in LAYOUT_TYPES:
            print(f"  sizeof({t}) = {sizes[t]}")
        return 0


def extract_sdk_prototypes(sdk_text: str, symbols: list[str]) -> list[str]:
    """Pull full function declarations for allowlisted symbols from the SDK header."""
    # Strip block comments to simplify matching.
    stripped = re.sub(r"/\*.*?\*/", "", sdk_text, flags=re.S)
    stripped = re.sub(r"//.*?$", "", stripped, flags=re.M)
    protos: list[str] = []
    for sym in symbols:
        if sym in LIBC_OR_LIBGCC:
            continue
        # Match from type start through closing ); before next decl.
        m = re.search(
            rf"(?:^|\n)([a-zA-Z_][\w\s\*]*?\b{re.escape(sym)}\s*\([^;]*\))\s*;",
            stripped,
        )
        if m:
            proto = " ".join(m.group(1).split())
            protos.append(proto + ";")
    return protos


def emit_proto_check(sdk: Path, allowlist: Path, out: Path) -> int:
    text = load_sdk_text(sdk)
    symbols = load_allowlist(allowlist)
    protos = extract_sdk_prototypes(text, symbols)
    if not protos:
        print("check_plugin_sdk: no prototypes extracted", file=sys.stderr)
        return 1
    lines = [
        "/* Auto-generated by tools/check_plugin_sdk.py — do not edit */",
        "/* Redeclare SDK prototypes after Host headers; mismatch => compile error. */",
        "",
        '#include "esp_err.h"',
        '#include "esp_log.h"',
        '#include "esp_random.h"',
        '#include "esp_timer.h"',
        '#include "esp_crt_bundle.h"',
        '#include "esp_heap_caps.h"',
        '#include "esp_http_client.h"',
        '#include "cJSON.h"',
        '#include "nvs.h"',
        '#include "mbedtls/base64.h"',
        '#include "mbedtls/md.h"',
        '#include "psa/crypto.h"',
        '#include "lwip/sockets.h"',
        '#include "lwip/inet.h"',
        '#include "freertos/FreeRTOS.h"',
        '#include "freertos/task.h"',
        '#include "freertos/queue.h"',
        '#include "freertos/semphr.h"',
        '#include "freertos/idf_additions.h"',
        '#include "minihub_core.h"',
        '#include "minihub_adapter.h"',
        '#include "minihub_module_abi.h"',
        '#include "minihub_module.h"',
        '#include "minihub_config.h"',
        '#include "minihub_hub.h"',
        '#include "minihub_net.h"',
        '#include "minihub_http_worker.h"',
        '#include "minihub_ble_gatt.h"',
        '#include "plugin_sdk_sizes.h"',
        "",
        "#include <stddef.h>",
        "",
        "/* Layout: Host sizeof must match SDK-only sizeof (same cross-compiler). */",
    ]
    for t in LAYOUT_TYPES:
        lines.append(
            f'_Static_assert(sizeof({t}) == MH_SDK_SIZE_{t}, '
            f'"Plugin SDK ABI drift: sizeof({t}) != minihub_plugin_sdk.h");'
        )
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append('extern "C" {')
    lines.append("#endif")
    lines.append("")
    lines.append("/* Function prototypes from minihub_plugin_sdk.h (must match Host). */")
    for p in protos:
        # Skip minihub_module_entry — plugin exports it; Host may not declare it.
        if "minihub_module_entry" in p:
            continue
        lines.append(p)
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    # Ensure the TU is not empty for some compilers.
    lines.append("void minihub_plugin_sdk_abi_check_anchor(void) {}")
    lines.append("")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"check_plugin_sdk: wrote {out} ({len(protos)} prototypes)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=("coverage", "emit-sizes", "emit-check", "check"))
    ap.add_argument("--sdk", type=Path, default=DEFAULT_SDK)
    ap.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--sdk-inc", type=Path, default=DEFAULT_SDK.parent)
    ap.add_argument("--sizes-out", type=Path, default=None)
    ap.add_argument("--check-out", type=Path, default=None)
    ap.add_argument(
        "--cflags",
        default="",
        help="Extra compiler flags for emit-sizes (e.g. Host sdkconfig include). "
        "Space-separated; passed verbatim to the probe compile.",
    )
    args = ap.parse_args()
    cflags = tuple(args.cflags.split()) if args.cflags else ()

    if args.command == "coverage":
        return check_coverage(args.sdk, args.allowlist)

    if args.command == "emit-sizes":
        out = args.sizes_out or Path("plugin_sdk_sizes.h")
        return emit_sizes(args.cc, args.sdk_inc, out, cflags)

    if args.command == "emit-check":
        sizes = args.sizes_out or Path("plugin_sdk_sizes.h")
        check = args.check_out or Path("plugin_sdk_abi_check.c")
        rc = check_coverage(args.sdk, args.allowlist)
        if rc:
            return rc
        rc = emit_sizes(args.cc, args.sdk_inc, sizes, cflags)
        if rc:
            return rc
        return emit_proto_check(args.sdk, args.allowlist, check)

    if args.command == "check":
        # coverage-only convenience (no compiler)
        return check_coverage(args.sdk, args.allowlist)

    return 2


if __name__ == "__main__":
    sys.exit(main())

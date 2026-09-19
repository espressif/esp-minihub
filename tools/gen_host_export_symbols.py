#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Generate host ELF symbol exports for dynamic adapter .so relocation."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ALLOWLIST_JSON = ROOT / "docs" / "module_sdk" / "host_export_allowlist.json"

# Soft-float / libgcc helpers referenced by adapter .so (no public headers).
LIBGCC_EXTERNS: dict[str, str] = {
    "__divdi3": "extern long long __divdi3(long long, long long);",
    "__extendsfdf2": "extern double __extendsfdf2(float);",
    "__fixunsdfsi": "extern unsigned int __fixunsdfsi(double);",
    "__floatsidf": "extern double __floatsidf(int);",
    "__floatunsidf": "extern double __floatunsidf(unsigned int);",
    "__fixdfsi": "extern int __fixdfsi(double);",
    "__truncdfsf2": "extern float __truncdfsf2(double);",
    "__adddf3": "extern double __adddf3(double, double);",
    "__subdf3": "extern double __subdf3(double, double);",
    "__muldf3": "extern double __muldf3(double, double);",
    "__divdf3": "extern double __divdf3(double, double);",
    "__fixdfdi": "extern long long __fixdfdi(double);",
    "__floatdidf": "extern double __floatdidf(long long);",
}


def _tool_from_gcc(name: str) -> str | None:
    for gcc_name in ("riscv32-esp-elf-gcc", "xtensa-esp-elf-gcc", "gcc"):
        gcc = shutil.which(gcc_name)
        if not gcc:
            continue
        try:
            probe = subprocess.run(
                [gcc, f"-print-prog-name={name}"],
                capture_output=True,
                text=True,
                check=False,
            )
        except OSError:
            continue
        candidate = probe.stdout.strip()
        if probe.returncode == 0 and candidate and candidate != name:
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                return candidate
    return None


def _tool_from_espressif_dir(name: str) -> str | None:
    triples = ("riscv32-esp-elf", "xtensa-esp-elf")
    tools_root = Path.home() / ".espressif" / "tools"
    if not tools_root.is_dir():
        return None
    for triple in triples:
        for bin_dir in sorted(tools_root.glob(f"{triple}/*/{triple}/bin"), reverse=True):
            candidate = bin_dir / f"{triple}-{name}"
            if candidate.is_file():
                return str(candidate)
    return None


def find_tool(name: str) -> str:
    for candidate in (
        shutil.which(name),
        shutil.which(f"riscv32-esp-elf-{name}"),
        shutil.which(f"xtensa-esp-elf-{name}"),
        _tool_from_gcc(name),
        _tool_from_espressif_dir(name),
    ):
        if candidate:
            return candidate
    raise RuntimeError(
        f"{name} not found. Install ESP-IDF toolchain or run: source $IDF_PATH/export.sh"
    )


def extract_elf_bytes(data: bytes) -> bytes | None:
    """Return ELF payload from raw ELF, MHMD+ELF, or .mhub zip bytes."""
    if data[:2] == b"PK":
        import io
        import zipfile

        try:
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                names = zf.namelist()
                so_names = [n for n in names if n.endswith(".so") and not n.endswith("/")]
                if not so_names:
                    return None
                # Prefer archive-root <id>.so over nested paths.
                so_names.sort(key=lambda n: (n.count("/"), len(n), n))
                data = zf.read(so_names[0])
        except zipfile.BadZipFile:
            return None
    idx = data.find(b"\x7fELF")
    if idx < 0:
        return None
    return data[idx:]


def extract_mhub_elf(mhub: Path, dest: Path) -> bool:
    elf = extract_elf_bytes(mhub.read_bytes())
    if elf is None:
        return False
    dest.write_bytes(elf)
    return True


def materialize_elf(path: Path, dest: Path) -> bool:
    """Write a readelf-able ELF extracted from path (raw / MHMD / .mhub)."""
    return extract_mhub_elf(path, dest)


def undefined_symbols(so_path: Path) -> list[str]:
    readelf = find_tool("readelf")
    # Adapter artifacts may be MHMD-wrapped; readelf needs a bare ELF.
    work = so_path
    tmp: Path | None = None
    raw = so_path.read_bytes()
    if not raw.startswith(b"\x7fELF"):
        import tempfile

        fd, tmp_name = tempfile.mkstemp(prefix="mh_elf_", suffix=".so")
        os.close(fd)
        tmp = Path(tmp_name)
        if not materialize_elf(so_path, tmp):
            tmp.unlink(missing_ok=True)
            return []
        work = tmp
    try:
        out = subprocess.check_output(
            [readelf, "-s", "-W", str(work)], text=True, stderr=subprocess.STDOUT
        )
    finally:
        if tmp is not None and tmp.is_file():
            tmp.unlink(missing_ok=True)
    pattern = re.compile(r"\s+\d+:\s+\S+\s+\d+\s+NOTYPE\s+GLOBAL\s+DEFAULT\s+UND\s+(\S+)")
    names = sorted({m.group(1) for m in pattern.finditer(out)})
    return [n for n in names if n and n != "UND"]


def collect_symbols(so_paths: list[Path]) -> tuple[list[str], str]:
    merged: set[str] = set()
    labels: list[str] = []
    for so_path in so_paths:
        if not so_path.is_file():
            continue
        syms = undefined_symbols(so_path)
        if not syms and not so_path.read_bytes().startswith(b"\x7fELF"):
            # Skip unreadable wrappers without treating as hard failure later.
            continue
        merged.update(syms)
        labels.append(so_path.name)
    if not merged:
        return [], ""
    comment = ", ".join(labels) if labels else so_paths[0].name
    return sorted(merged), comment


def _symbol_groups(symbols: list[str]) -> dict[str, list[str]]:
    groups: dict[str, list[str]] = {
        "common": [],
        "ble_gatt": [],
        "ble_mesh": [],
        "zigbee": [],
        "module": [],
        "net": [],
    }
    for sym in symbols:
        if sym.startswith("minihub_ble_gatt_"):
            groups["ble_gatt"].append(sym)
        elif sym.startswith("minihub_ble_mesh_"):
            groups["ble_mesh"].append(sym)
        elif sym.startswith("minihub_zigbee_") or sym.startswith("minihub_zb_"):
            groups["zigbee"].append(sym)
        elif sym.startswith("minihub_module_"):
            groups["module"].append(sym)
        elif sym.startswith("minihub_net_"):
            groups["net"].append(sym)
        else:
            groups["common"].append(sym)
    return groups


# Public Module SDK allowlist excludes radio adapter private APIs.
PUBLIC_ALLOWLIST_DENY_PREFIXES = (
    "minihub_ble_mesh_",
    "minihub_zigbee_",
    "minihub_zb_",
    "esp_wifi_",
    "esp_ble_",
    "zb_",
)


def is_public_allowlist_symbol(sym: str) -> bool:
    return not any(sym.startswith(p) for p in PUBLIC_ALLOWLIST_DENY_PREFIXES)


def render_allowlist_json(symbols: list[str], source_label: str) -> str:
    public = sorted(s for s in symbols if is_public_allowlist_symbol(s))
    denied = sorted(s for s in symbols if not is_public_allowlist_symbol(s))
    doc = {
        "description": (
            "Snapshot of Host ELF exports a Store .mhub may leave undefined; "
            "Host resolves them via esp_elf_register_symbol (same set as "
            "host_elf_symbols_generated.c + host_elf_symbols.c). "
            "Addresses are firmware-local — do not hardcode. "
            "Compatibility: module abi_version + relocate failure (no separate export ABI)."
        ),
        "scope": "IP plugins (HTTP/TLS, UDP sockets, crypto, NVS, cJSON)",
        "excluded": "Wi-Fi / Zigbee / BLE / Matter radio control APIs; httpd URI registration",
        "source": source_label,
        "symbols": public,
    }
    if denied:
        doc["omitted_from_public"] = denied
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def render_c(symbols: list[str], so_name: str) -> str:
    groups = _symbol_groups(symbols)
    libgcc = [s for s in groups["common"] if s in LIBGCC_EXTERNS or s.startswith("__")]

    lines = [
        "/* Auto-generated by tools/gen_host_export_symbols.py — do not edit */",
        "/* SPDX-FileCopyrightText: 2026 MiniHub Contributors */",
        "/* SPDX-License-Identifier: Apache-2.0 */",
        "",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "#include <strings.h>",
        "#include <sys/stat.h>",
        "#include <time.h>",
        "#include <unistd.h>",
        '#include "esp_elf.h"',
        '#include "esp_err.h"',
        '#include "esp_log.h"',
        '#include "esp_random.h"',
        '#include "esp_timer.h"',
        '#include "esp_crt_bundle.h"',
        '#include "esp_http_client.h"',
        '#include "cJSON.h"',
        '#include "mbedtls/base64.h"',
        '#include "mbedtls/md.h"',
        '#include "psa/crypto.h"',
        '#include "nvs_flash.h"',
        '#include "lwip/inet.h"',
        '#include "lwip/sockets.h"',
        '#include "freertos/FreeRTOS.h"',
        '#include "freertos/semphr.h"',
        '#include "minihub_core.h"',
        '#include "minihub_http_worker.h"',
    ]
    if "heap_caps_get_free_size" in symbols:
        lines.append('#include "esp_heap_caps.h"')
    if groups["ble_gatt"]:
        lines.append('#include "minihub_ble_gatt.h"')
    if groups["ble_mesh"]:
        lines.append('#include "minihub_ble_mesh.h"')
    if groups["zigbee"]:
        lines.append('#include "minihub_zigbee.h"')
    if groups["module"]:
        lines.append('#include "minihub_module.h"')
    if groups["net"]:
        lines.append('#include "minihub_net.h"')
    lines.append("")

    for sym in sorted(set(libgcc)):
        if sym in LIBGCC_EXTERNS:
            lines.append(LIBGCC_EXTERNS[sym])
        else:
            # Unknown libgcc helper — cast via void* export needs a declaration.
            lines.append(f"extern void {sym}(void);")
    if libgcc:
        lines.append("")

    lines.append(f"/* Undefined symbols from {so_name} */")
    lines.append("const struct esp_elfsym s_minihub_module_dep_elfsyms[] = {")

    for sym in (
        groups["common"]
        + groups["ble_gatt"]
        + groups["ble_mesh"]
        + groups["zigbee"]
        + groups["module"]
    ):
        lines.append(f"    ESP_ELFSYM_EXPORT({sym}),")

    lines += [
        "    ESP_ELFSYM_END",
        "};",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--so", action="append", type=Path, help="adapter .so path (repeatable)")
    parser.add_argument("--modules-dir", type=Path, help="directory with libadapter_*.so")
    parser.add_argument(
        "--mhmd-dir",
        type=Path,
        help="directory with MHMD intermediates (<id>.so from pack_store_module.py mhmd)",
    )
    parser.add_argument(
        "--store-dir",
        type=Path,
        help="optional deploy root — merge UND from */*.mhub",
    )
    parser.add_argument("--out", required=True, type=Path, help="output .c path")
    parser.add_argument(
        "--allowlist-json",
        type=Path,
        default=DEFAULT_ALLOWLIST_JSON,
        help="docs snapshot of exported symbol names (Module SDK)",
    )
    parser.add_argument(
        "--skip-allowlist",
        action="store_true",
        help="do not write docs/module_sdk/host_export_allowlist.json",
    )
    args = parser.parse_args()

    so_paths: list[Path] = list(args.so or [])
    if args.modules_dir:
        so_paths.extend(sorted(args.modules_dir.glob("libadapter_*.so")))
    if args.mhmd_dir and args.mhmd_dir.is_dir():
        so_paths.extend(
            sorted(
                p
                for p in args.mhmd_dir.glob("*.so")
                if p.is_file() and not p.name.startswith("libadapter_")
            )
        )
    so_paths = [p for p in so_paths if p.is_file()]

    tmp_extracted: list[Path] = []
    if args.store_dir and args.store_dir.is_dir():
        import tempfile

        tmpdir = Path(tempfile.mkdtemp(prefix="mhub_syms_"))
        mhubs = sorted(args.store_dir.glob("*.mhub"))
        mhubs.extend(sorted(args.store_dir.glob("*/*.mhub")))
        for mhub in mhubs:
            if not mhub.is_file():
                continue
            dest = tmpdir / f"{mhub.stem}.so"
            if extract_mhub_elf(mhub, dest):
                so_paths.append(dest)
                tmp_extracted.append(dest)

    if not so_paths:
        print("error: no adapter .so files found", file=sys.stderr)
        return 1

    # Always keep symbols needed by currently shipped modules even if a rebuild
    # of .so dropped them (device may still have older .mhub in modstore).
    baseline = {
        "esp_timer_get_time",
        "esp_log",
        "esp_log_timestamp",
        "esp_err_to_name",
        "heap_caps_get_free_size",
    }

    symbols, label = collect_symbols(so_paths)
    symbols = sorted(set(symbols) | baseline)
    if not symbols:
        print(f"warning: no UND symbols in {label}", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render_c(symbols, label), encoding="utf-8")
    print(f"gen_host_export_symbols: {len(symbols)} symbols from {len(so_paths)} .so -> {args.out}")

    if not args.skip_allowlist:
        # Always-exported Host helpers (host_elf_symbols.c) belong on the public list.
        always = {
            "minihub_config_get_adapters",
            "minihub_config_save_adapters",
            "minihub_hub_reload",
            "esp_timer_get_time",
            "minihub_net_dns_probe",
            "heap_caps_get_free_size",
            "mkdir",
            "stat",
            "unlink",
            "rmdir",
            "esp_http_client_open",
            "esp_http_client_fetch_headers",
            "esp_http_client_get_content_length",
            "esp_http_client_read",
            "esp_http_client_close",
            "minihub_point_list_by_adapter",
            "minihub_point_list_free",
            "minihub_command_invoke",
        }
        allow_syms = sorted(set(symbols) | always)
        allow_path = args.allowlist_json
        allow_path.parent.mkdir(parents=True, exist_ok=True)
        allow_path.write_text(
            render_allowlist_json(allow_syms, label),
            encoding="utf-8",
        )
        public_n = sum(1 for s in allow_syms if is_public_allowlist_symbol(s))
        print(f"gen_host_export_symbols: allowlist public={public_n} -> {allow_path}")

    for p in tmp_extracted:
        try:
            p.unlink(missing_ok=True)
            p.parent.rmdir()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

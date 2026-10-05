#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""ABI contract core: parse the Host vtable + allowlist, compute per-symbol
hashes, and derive the host-side acceptable-hash set.

This module is the single source of truth shared by:

  * tools/gen_abi_contract.py  -- generate host_abi_versions.json + baked header
  * tools/store_server.py      -- validate a plugin's ``abi.symbols`` at request time
  * tools/sync_store_catalog.py-- stamp the ``abi`` block onto a plugin catalog.json

Hash rule (mirrors the plan): a symbol's hash is
``H(position \\0 name \\0 signature)`` where signature is the C type/signature.
Name changes, adjacent swaps (position change) and type changes all alter the
hash, so they are detected as "different == incompatible". The host keeps, per
symbol, the set of *acceptable* hashes (the current hash plus historical
compatible aliases) so older plugins keep working.

The plugin side never recomputes the signature: it copies the host's hash for
each undefined host symbol it links against (computed here at pack time), so the
two sides are always in agreement.
"""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import tempfile
import zipfile
from pathlib import Path

# vtable host api struct in minihub_module_abi.h
DEFAULT_HEADER = "components/minihub_module/include/minihub_module_abi.h"
DEFAULT_ALLOWLIST = "docs/module_sdk/host_export_allowlist.json"

ABI_VERSIONS_SCHEMA = "minihub.abi.versions/1"
HASH_PREFIX = "sha256:"


def _hash(text: str) -> str:
    return HASH_PREFIX + hashlib.sha256(text.encode("utf-8")).hexdigest()


_FNPTR_RE = re.compile(
    r"^\s*([\w\s\*]+?)\s*\(\s*\*([A-Za-z_]\w*)\s*\)\s*\(([^;]*?)\)\s*;\s*$"
)
_SCALAR_RE = re.compile(r"^\s*([\w\s\*]+?)\s+([A-Za-z_]\w*)\s*;\s*$")


def _normalize_args(args: str) -> str:
    """Drop parameter names, keep only types, so ``const char *mac`` and
    ``const char *name`` hash the same way structurally (names are not part of
    the ABI contract; only position + types are)."""
    if args.strip() == "void":
        return "void"
    out = []
    for arg in args.split(","):
        arg = arg.strip()
        if not arg:
            continue
        toks = arg.split()
        # Drop a trailing bare identifier that is a parameter name (a type token
        # would be followed by '*' or be the last type keyword, not a lone word).
        if len(toks) > 1 and toks[-1] not in ("*", "&", "void") and not toks[-1].endswith("*"):
            toks = toks[:-1]
        out.append(" ".join(toks))
    return ", ".join(out)


def parse_host_api_header(header: Path) -> list[tuple[str, str]]:
    """Return ``[(name, signature), ...]`` for every member of
    ``minihub_module_host_api_t``, in declaration order (position matters)."""
    text = header.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"typedef struct\s*\{(.*?)\}\s*minihub_module_host_api_t\s*;", text, re.S)
    if not m:
        raise RuntimeError(f"minihub_module_host_api_t not found in {header}")
    body = m.group(1)
    fields: list[tuple[str, str]] = []
    for raw in body.splitlines():
        if not raw.strip() or raw.strip().startswith("/*") or raw.strip().startswith("*"):
            continue
        fn = _FNPTR_RE.match(raw)
        if fn:
            ret = " ".join(fn.group(1).split())
            args = _normalize_args(fn.group(3))
            fields.append((fn.group(2), f"{ret}({args})"))
            continue
        sc = _SCALAR_RE.match(raw)
        if sc:
            typ = " ".join(sc.group(1).split())
            fields.append((sc.group(2), typ))
    if not fields:
        raise RuntimeError(f"no members parsed from minihub_module_host_api_t in {header}")
    return fields


def merge_allowlist(symbols: dict[str, str], allowlist: Path) -> dict[str, str]:
    """Add Host ELF export symbols (the allowlist) to the symbol set.

    Allowlist entries have no signature available, so their hash is just the
    name. Plugins reference exactly these symbols through undefined ELF symbols.
    """
    if not allowlist.is_file():
        return symbols
    obj = json.loads(allowlist.read_text(encoding="utf-8"))
    names = obj.get("symbols") or []
    for name in names:
        if isinstance(name, str) and name not in symbols:
            symbols[name] = _hash(name)
    return symbols


def parse_abi_version(header: Path = Path(DEFAULT_HEADER)) -> int:
    """Return the ``MINIHUB_MODULE_ABI_VERSION`` constant from the host header."""
    text = header.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"#define\s+MINIHUB_MODULE_ABI_VERSION\s+(\d+)", text)
    return int(m.group(1)) if m else 1


def build_host_symbols(
    header: Path = Path(DEFAULT_HEADER),
    allowlist: Path = Path(DEFAULT_ALLOWLIST),
) -> dict[str, str]:
    """Ordered ``name -> hash`` for every Host API the plugin may use."""
    fields = parse_host_api_header(header)
    symbols: dict[str, str] = {}
    for pos, (name, sig) in enumerate(fields):
        symbols[name] = _hash(f"{pos}\x00{name}\x00{sig}")
    return merge_allowlist(symbols, allowlist)


def compute_abi_total_hash(symbols: dict[str, str]) -> str:
    """Hash of every (name, hash) pair in canonical (sorted-by-name) order."""
    canonical = "".join(f"{n}\x00{symbols[n]}\x00" for n in sorted(symbols))
    return _hash(canonical)


def plugin_symbol_block(mhub: Path, host_symbols: dict[str, str]) -> dict[str, str]:
    """For a built ``.mhub``, return ``{symbol: host_hash}`` for every undefined
    Host symbol it links against. The hash is copied verbatim from the Host's
    current contract -- this is what makes host/server/device agree.

    The ``.mhub`` may be either a raw MHMD+ELF image or a zip container holding
    the ``.so``; in the latter case the ELF is extracted to a temp file first.
    """
    if not mhub.is_file():
        return {}
    target = mhub
    tmp = None
    if zipfile.is_zipfile(mhub):
        try:
            with zipfile.ZipFile(mhub) as zf:
                so = next((n for n in zf.namelist() if n.endswith(".so") or n.endswith(".elf")), None)
                if so:
                    tmp = Path(tempfile.mkdtemp()) / "mod.so"
                    tmp.write_bytes(zf.read(so))
                    target = tmp
        except zipfile.BadZipFile:
            pass
    try:
        und = _read_und_symbols(target)
    finally:
        if tmp is not None and tmp.exists():
            try:
                tmp.unlink()
                tmp.parent.rmdir()
            except OSError:
                pass
    block: dict[str, str] = {}
    for name in und:
        key = name
        # macOS/clang prepends a leading underscore to C symbols; strip a single
        # one only when the de-prefixed name matches a host symbol. On Linux ELF
        # (the real ESP toolchain) names have no prefix, so this is a no-op there.
        if key not in host_symbols and key.startswith("_") and key[1:] in host_symbols:
            key = key[1:]
        if key in host_symbols:
            block[key] = host_symbols[key]
    return block


def _read_und_symbols(mhub: Path) -> set[str]:
    """Collect undefined symbol names from an ELF.

    Tries, in order: readelf (Linux toolchain), ``nm -D -u`` (dynamic undefined),
    plain ``nm`` (lines with a ``U`` type), and ``objdump -t``. The first that
    yields symbols wins.
    """
    for cmd in (
        ["readelf", "-sW", str(mhub)],
        ["readelf", "-s", str(mhub)],
        ["nm", "-D", "-u", str(mhub)],
        ["nm", str(mhub)],
        ["objdump", "-t", str(mhub)],
    ):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            continue
        if out.returncode != 0:
            continue
        names: set[str] = set()
        for line in out.stdout.splitlines():
            if " UND " in line:
                parts = line.split()
                if parts:
                    names.add(parts[-1])
                continue
            # plain `nm`/`objdump -t`: "addr T type name" with type 'U'
            toks = line.split()
            if len(toks) >= 2 and toks[-2] == "U":
                names.add(toks[-1])
        if names:
            return names
    print(f"[abi] warn: could not read undefined symbols from {mhub}", flush=True)
    return set()





def write_baked_header(
    path: Path,
    abi_version: int,
    abi_total_hash: str,
    symbols: dict[str, str],
    *,
    acceptable: dict[str, list[str]] | None = None,
) -> None:
    """Write the C header baked into firmware for offline compatibility checks.

    ``acceptable`` maps each symbol name to its list of accepted hashes
    (current + any historical alias hashes). Defaults to the single current hash.
    """
    if acceptable is None:
        acceptable = {n: [h] for n, h in symbols.items()}
    lines = [
        "/* Generated by tools/gen_abi_contract.py -- do not edit. */",
        "#pragma once",
        f'#define MINIHUB_ABI_TOTAL_HASH "{abi_total_hash}"',
        f"#define MINIHUB_ABI_VERSION_BAKED {abi_version}",
        "typedef struct {",
        "    const char *name;",
        "    const char *const *hashes;   /* acceptable hashes (current + aliases) */",
        "    size_t n_hashes;",
        "} minihub_abi_symbol_t;",
    ]
    idx = 0
    for name in sorted(symbols):
        hashes = acceptable.get(name, [symbols[name]])
        arr = ", ".join(f'"{h}"' for h in hashes) or "NULL"
        lines.append(f"static const char *const kMhAbiH{idx}[] = {{{arr}}};")
        lines.append(f'static const minihub_abi_symbol_t kMhAbiSym{idx} = {{"{name}", kMhAbiH{idx}, {len(hashes)}}};')
        idx += 1
    lines.append("static const minihub_abi_symbol_t *const kMinihubAbiSymbols[] = {")
    for i in range(idx):
        lines.append(f"    &kMhAbiSym{i},")
    lines += [
        "};",
        "#define MINIHUB_ABI_SYMBOL_COUNT "
        "(sizeof(kMinihubAbiSymbols) / sizeof(kMinihubAbiSymbols[0]))",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")

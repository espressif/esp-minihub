#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Pack examples/plugins/<id>/ (or MINIHUB_PLUGIN_DIRS) into a Store ``.mhub``, or wrap native .so (MHMD).

Every module is published as one ``.mhub``: L1/L2 under
``deploy/Stores/Common/<id>/<id>.mhub``, native/hybrid under
``deploy/Stores/<chip>/<id>/<id>.mhub`` (``<chip>`` from ``targets``).

Two orthogonal steps (do not conflate):

  pack  — outer Store package → ``<id>.mhub`` (container format, default zip)
  mhmd  — inner native image  → MHMD header + ELF (not a compression format)

L1 (config_only): module.json only
L2 (runtime js):  module.json + main.js (+ ui/web)
L3 (native):      module.json + <id>.so (MHMD+ELF from build/<id>.so) (+ ui/web)
Hybrid:           module.json + <id>.so + main.js (+ ui/web)

Source module.json stays declarative (no binary_size / adapter_version sizes).
Packaged module.json may include binary_size / adapter_version for on-device checks.
Deploy dir must not contain bare <id>.so — only <id>.mhub.

Container format: ``--format`` / ``MHUB_PACK_FORMAT`` (default ``zip``).
Host ``hub_zip.c`` currently unpacks zip only; other formats need firmware support first.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import zipfile
from pathlib import Path

# Outer .mhub container (not MHMD). Extend when Host gains another unpacker.
PACK_FORMATS = ("zip",)
DEFAULT_PACK_FORMAT = "zip"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import (  # noqa: E402
    find_deploy_mhubs,
    iter_module_source_dirs,
    module_mhmd_path,
    module_source_dir,
    prune_plugin_dirs,
    resolve_deploy_dir,
)

# Build-derived keys — never required in source-tree module.json
ARTIFACT_KEYS = ("hub_size", "binary_size", "adapter_version")

# MHMD header (native .so payload for dynamic loader)
MHMD_MAGIC = 0x4D484D44
MHMD_ABI_VERSION = 1
MHMD_CORE_API_VERSION = 1


def wrap_mhmd(so_path: Path, out_path: Path) -> int:
    """Wrap a linked .so with MiniHub MHMD header → build/<id>.so intermediate."""
    payload = so_path.read_bytes()
    header = struct.pack(
        "<IIIIII12s",
        MHMD_MAGIC,
        MHMD_ABI_VERSION,
        len(payload),
        0,
        MHMD_CORE_API_VERSION,
        MHMD_CORE_API_VERSION,
        b"\0" * 12,
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + payload)
    print(f"Wrote {out_path} ({len(header) + len(payload)} bytes)")
    return len(header) + len(payload)


def is_hybrid_module(manifest: dict) -> bool:
    return manifest.get("runtime") == "hybrid"


def is_config_only(manifest: dict) -> bool:
    if manifest.get("config_only") is True:
        return True
    if manifest.get("runtime") == "pack":
        return True
    if manifest.get("binary") or manifest.get("runtime") in ("js", "native", "hybrid"):
        return False
    if manifest.get("inject") or manifest.get("requires"):
        return True
    return False


def is_js_module(manifest: dict, module_dir: Path) -> bool:
    if is_hybrid_module(manifest):
        return False
    if manifest.get("runtime") == "js":
        return True
    if manifest.get("runtime") == "native":
        return False
    if (module_dir / "CMakeLists.txt").is_file():
        return False
    return (module_dir / "main.js").is_file()


def read_adapter_version(module_dir: Path, module_id: str) -> str:
    import re

    candidates = [
        module_dir / f"{module_id}_adapter.c",
        *sorted(module_dir.glob("*_adapter.c")),
    ]
    for path in candidates:
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        match = re.search(r"\.version\s*=\s*\"([^\"]+)\"", text)
        if match:
            return match.group(1)
    return ""


def source_manifest(manifest: dict) -> dict:
    """Copy for source hygiene — drop deploy-only size fields if present."""
    out = {k: v for k, v in manifest.items() if k not in ARTIFACT_KEYS}
    return out


def resolve_native_so(module_id: str, *, root: Path, deploy: Path) -> Path:
    """Prefer build/<id>.so; else recover MHMD from an existing deploy .mhub."""
    so = module_mhmd_path(module_id, root=root)
    if so.is_file():
        return so
    name = f"{module_id}.so"
    for mhub in find_deploy_mhubs(module_id, deploy):
        with zipfile.ZipFile(mhub) as zf:
            if name in zf.namelist():
                so.parent.mkdir(parents=True, exist_ok=True)
                so.write_bytes(zf.read(name))
                return so
    raise FileNotFoundError(
        f"missing {so} — build/pack MHMD .so first (tools/build_store_modules.py)"
    )


def packaged_manifest(
    manifest: dict,
    *,
    module_dir: Path,
    module_id: str,
    so: Path | None,
) -> dict:
    """Manifest written into .mhub zip (may include binary_size for device checks)."""
    out = source_manifest(manifest)
    # ui + capabilities live in meta.json; module.json keeps only runtime/env info.
    out.pop("capabilities", None)
    out.pop("ui", None)
    out.pop("ui_file", None)
    need_binary = (
        so
        and so.is_file()
        and not is_config_only(out)
        and (is_hybrid_module(out) or not is_js_module(out, module_dir))
    )
    if need_binary:
        out["binary"] = f"{module_id}.so"
        out["binary_size"] = so.stat().st_size
        av = read_adapter_version(module_dir, module_id)
        if av:
            out["adapter_version"] = av
    from sync_store_catalog import plugin_targets

    out["targets"] = plugin_targets(manifest, module_dir)
    return out


def pack_store_module(
    module_id: str,
    *,
    root: Path | None = None,
    deploy: Path | None = None,
    compress: bool = True,
) -> Path | None:
    from store_paths import ROOT

    root = root or ROOT
    deploy = resolve_deploy_dir(deploy)
    module_dir = module_source_dir(module_id, root=root)
    manifest_path = module_dir / "module.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"missing {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    mid = manifest.get("id") or module_id
    if mid != module_id:
        raise ValueError(f"module.json id={mid!r} != directory {module_id!r}")

    from sync_store_catalog import store_buckets_for_module

    buckets = store_buckets_for_module(manifest, module_dir)
    if not buckets:
        print(f"skip {module_id}: native module is not published to the store")
        return None
    deploy_dirs = [deploy / "Stores" / bucket / module_id for bucket in buckets]
    deploy_mod = deploy_dirs[0]
    deploy_mod.mkdir(parents=True, exist_ok=True)
    pkg_path = deploy_mod / f"{module_id}.mhub"
    compression = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
    names: list[str] = []

    so: Path | None = None
    hybrid = is_hybrid_module(manifest)
    if not is_config_only(manifest) and (hybrid or not is_js_module(manifest, module_dir)):
        so = resolve_native_so(module_id, root=root, deploy=deploy)

    pkg_manifest = packaged_manifest(
        manifest, module_dir=module_dir, module_id=module_id, so=so
    )

    # meta.json: canonical home for ui + capabilities (module.json keeps only
    # runtime/environment info). Read meta.json from the module dir (preferred);
    # fall back to legacy module.json capabilities + ui.json for old sources.
    meta: dict = {}
    meta_path = module_dir / "meta.json"
    if meta_path.is_file():
        try:
            loaded = json.loads(meta_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                meta = loaded
        except (OSError, json.JSONDecodeError) as exc:
            print(f"WARN: bad {meta_path}: {exc}", file=sys.stderr)
    if "capabilities" not in meta:
        caps = manifest.get("capabilities")
        if isinstance(caps, list):
            meta["capabilities"] = caps
    if "ui" not in meta:
        if isinstance(manifest.get("ui"), dict):
            meta["ui"] = manifest["ui"]
        else:
            ui_fname = manifest.get("ui_file") or "ui.json"
            ui_file = module_dir / ui_fname
            if ui_file.is_file():
                try:
                    meta["ui"] = json.loads(ui_file.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    print(f"WARN: bad {ui_file}: {exc}", file=sys.stderr)

    with zipfile.ZipFile(pkg_path, "w", compression=compression) as zf:
        zf.writestr(
            "module.json",
            json.dumps(pkg_manifest, indent=2, ensure_ascii=False) + "\n",
        )
        names.append("module.json")

        if meta:
            zf.writestr("meta.json", json.dumps(meta, indent=2, ensure_ascii=False) + "\n")
            names.append("meta.json")

        if is_config_only(manifest):
            pass
        elif is_js_module(manifest, module_dir):
            main_js = module_dir / "main.js"
            if not main_js.is_file():
                raise FileNotFoundError(f"L2 module missing {main_js}")
            zf.write(main_js, "main.js")
            names.append("main.js")
        elif hybrid:
            assert so is not None
            zf.write(so, f"{module_id}.so")
            names.append(f"{module_id}.so")
            main_js = module_dir / "main.js"
            if not main_js.is_file():
                raise FileNotFoundError(f"hybrid module missing {main_js}")
            zf.write(main_js, "main.js")
            names.append("main.js")
        else:
            assert so is not None
            zf.write(so, f"{module_id}.so")
            names.append(f"{module_id}.so")

        if not is_config_only(manifest):
            devices = module_dir / "devices.json"
            if devices.is_file():
                zf.write(devices, "devices.json")
                names.append("devices.json")
            catalog = module_dir / "catalog.json"
            if catalog.is_file():
                zf.write(catalog, "catalog.json")
                names.append("catalog.json")
            web = module_dir / "web"
            if web.is_dir():
                for path in sorted(web.rglob("*")):
                    if path.is_file():
                        arc = path.relative_to(module_dir).as_posix()
                        zf.write(path, arc)
                        names.append(arc)

    for extra in deploy_dirs[1:]:
        extra.mkdir(parents=True, exist_ok=True)
        shutil.copy2(pkg_path, extra / pkg_path.name)

    # Hard cutover: remove legacy .hub and any deploy-side MHMD .so
    for dest in deploy_dirs:
        legacy_hub = dest / f"{module_id}.hub"
        if legacy_hub.is_file():
            legacy_hub.unlink()
        stray_so = dest / f"{module_id}.so"
        if stray_so.is_file():
            stray_so.unlink()
    prune_plugin_dirs(deploy, module_id, buckets)

    from sync_store_catalog import write_packed_catalog

    write_packed_catalog(module_id, deploy)
    print(
        f"Packed {module_id}: {pkg_path} ({pkg_path.stat().st_size} bytes) [{', '.join(names)}]"
    )
    return pkg_path


def iter_packable_ids() -> list[str]:
    ids: list[str] = []
    seen: set[str] = set()
    for d in iter_module_source_dirs():
        if d.name not in seen:
            seen.add(d.name)
            ids.append(d.name)
    return ids


def module_is_native(module_id: str) -> bool:
    """True for L3 native and hybrid packages (they ship an ELF ``.so``)."""
    module_dir = module_source_dir(module_id)
    manifest_path = module_dir / "module.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if is_config_only(manifest):
        return False
    if is_hybrid_module(manifest):
        return True
    return not is_js_module(manifest, module_dir)


def _default_pack_format() -> str:
    return os.environ.get("MHUB_PACK_FORMAT", DEFAULT_PACK_FORMAT).strip().lower() or DEFAULT_PACK_FORMAT


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pack Store .mhub packages, or wrap native .so with MHMD."
    )
    sub = parser.add_subparsers(dest="cmd")

    p_pack = sub.add_parser(
        "pack",
        help="Pack plugin dir → Stores/<chip-or-Common>/<id>/<id>.mhub",
    )
    p_pack.add_argument("module_id", nargs="+", help="module id(s), or 'all'")
    p_pack.add_argument(
        "--format",
        choices=PACK_FORMATS,
        default=None,
        help="outer .mhub container format (default: zip / MHUB_PACK_FORMAT)",
    )
    p_pack.add_argument(
        "--store",
        action="store_true",
        help="for --format zip: ZIP_STORED (no deflate)",
    )
    p_pack.add_argument(
        "--continue-on-error",
        action="store_true",
        help="keep packing remaining modules when one fails (e.g. native .so not built yet)",
    )
    p_pack.add_argument("--deploy", type=Path, default=None, help="deploy root")
    p_pack.add_argument(
        "--skip-native",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=argparse.SUPPRESS,
    )

    p_mhmd = sub.add_parser(
        "mhmd",
        help="Wrap linked .so with MHMD header (native image, not package format)",
    )
    p_mhmd.add_argument("so_path", type=Path)
    p_mhmd.add_argument("out_path", type=Path)

    # Compat: bare `<id>…` → pack; legacy `zip` subcommand → pack
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "zip":
        argv = ["pack", *argv[1:]]
    if argv and argv[0] not in ("pack", "mhmd", "-h", "--help"):
        argv = ["pack", *argv]

    args = parser.parse_args(argv)
    if args.cmd == "mhmd":
        wrap_mhmd(args.so_path, args.out_path)
        return 0
    if args.cmd != "pack":
        parser.print_help()
        return 2

    fmt = (args.format or _default_pack_format()).lower()
    if fmt not in PACK_FORMATS:
        print(
            f"unsupported MHUB_PACK_FORMAT/format={fmt!r}; "
            f"supported: {', '.join(PACK_FORMATS)}",
            file=sys.stderr,
        )
        return 2
    if fmt != "zip":
        # Reserved for future containers; Host currently unpacks zip only.
        print(f"pack format {fmt!r} not implemented yet", file=sys.stderr)
        return 2

    deploy = resolve_deploy_dir(args.deploy)
    ids = args.module_id
    if ids == ["all"]:
        found = iter_packable_ids()
        if not found:
            print("No modules with module.json found", file=sys.stderr)
            return 1
        ids = found
        if args.skip_native:
            kept: list[str] = []
            for mid in ids:
                try:
                    native = module_is_native(mid)
                except (OSError, ValueError):
                    native = False
                if not native:
                    kept.append(mid)
            ids = kept
            if not ids:
                return 0

    failed: list[str] = []
    for mid in ids:
        try:
            packed = pack_store_module(mid, deploy=deploy, compress=not args.store)
        except (OSError, ValueError, FileNotFoundError) as exc:
            print(f"pack_store_module {mid}: {exc}", file=sys.stderr)
            if args.continue_on_error:
                failed.append(mid)
                continue
            return 1
    if failed:
        print(
            f"packed {len(ids) - len(failed)}/{len(ids)}; "
            f"skipped (native .so not built?): {', '.join(failed)}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

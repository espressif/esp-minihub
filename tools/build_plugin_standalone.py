#!/usr/bin/env python3
"""Build a native/hybrid plugin with CMake + Plugin SDK (no ESP-IDF).

Plugin sources are resolved from (in order):

  * examples/plugins/<id>                  (in-tree demos)
  * MINIHUB_PLUGIN_DIRS entries            (":" / ";" separated; a single plugin
                                           dir or a parent holding several)

so a sibling Store repo (esp-minihub-plugins) can be built by exporting
MINIHUB_PLUGIN_DIRS at that checkout.

With ``--pack``, L3 goes through CMake target ``pack``; afterward L1/L2 under the
same roots are packed via ``pack_store_module.py`` (no cross-compiler).

Standalone consumers who only unzip module_sdk.zip should call cmake directly
with -DMINIHUB_MODULE_SDK_PATH=... — this script is a MiniHub-tree helper.
Override the SDK with --sdk or MINIHUB_MODULE_SDK_PATH when the zip is elsewhere.

Deploy artifacts (.mhub / catalog.json) go to MINIHUB_PLUGINS_DEPLOY_DIR
(default ~/workspace/minihub_store) — see tools/store_paths.py.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

def _here() -> Path:
    return Path(__file__).resolve().parent


def _detect_root() -> Path:
    """MiniHub repo root, or this directory when running from unzipped module_sdk.zip."""
    here = _here()
    if here.name == "tools" and (here.parent / "docs" / "module_sdk").is_dir():
        return here.parent
    if (here / "minihub_plugin_standalone.cmake").is_file():
        repo = here.parent.parent
        if (repo / "tools").is_dir() and (repo / "examples" / "plugins").is_dir():
            return repo
        return here
    return here.parent


ROOT = _detect_root()
sys.path.insert(0, str(_here()))
from store_paths import plugin_dir_env  # noqa: E402


def candidate_roots(
    root: Path = ROOT,
    plugins_dirs: list[Path] | None = None,
    include_demos: bool = True,
) -> list[Path]:
    """Plugin roots to search: in-tree demos first, then MINIHUB_PLUGIN_DIRS."""
    bases: list[Path] = []
    demos = root / "examples" / "plugins"
    if include_demos and demos.is_dir():
        bases.append(demos)
    bases.extend(plugins_dirs if plugins_dirs is not None else plugin_dir_env())
    out: list[Path] = []
    seen: set[str] = set()
    for b in bases:
        key = str(b)
        if key not in seen:
            seen.add(key)
            out.append(b)
    return out


def _is_buildable(d: Path) -> bool:
    """native/hybrid plugin that ships a CMakeLists.txt."""
    if not d.is_dir() or d.name.startswith((".", "_")):
        return False
    manifest = d / "module.json"
    if not manifest.is_file() or not (d / "CMakeLists.txt").is_file():
        return False
    try:
        info = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return (info.get("runtime") or "native") in ("native", "hybrid")


def _is_js_or_pack(d: Path) -> bool:
    """L1 pack / L2 js — module.json only, no CMake .so build."""
    if not d.is_dir() or d.name.startswith((".", "_")):
        return False
    manifest = d / "module.json"
    if not manifest.is_file():
        return False
    try:
        info = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    runtime = info.get("runtime") or "native"
    if runtime in ("js", "pack") or info.get("config_only") is True:
        return True
    if info.get("type") == "pack":
        return True
    return False


def discover_js_pack_ids(
    *,
    root: Path = ROOT,
    plugins_dirs: list[Path] | None = None,
    include_demos: bool = True,
) -> list[str]:
    """L1/L2 ids (no native .so) under the same roots as discover_module_ids."""
    ids: list[str] = []
    seen: set[str] = set()
    for base in candidate_roots(root, plugins_dirs, include_demos):
        entries = [base] if _is_js_or_pack(base) else sorted(
            p for p in base.iterdir() if p.is_dir()
        )
        for d in entries:
            if _is_js_or_pack(d) and d.name not in seen:
                seen.add(d.name)
                ids.append(d.name)
    return ids


def pack_store_ids(
    module_ids: list[str],
    *,
    root: Path = ROOT,
) -> None:
    """Zip L1/L2 (or re-pack) via tools/pack_store_module.py into deploy dir."""
    if not module_ids:
        return
    pack_py = _here() / "pack_store_module.py"
    if not pack_py.is_file():
        pack_py = root / "tools" / "pack_store_module.py"
    if not pack_py.is_file():
        sdk = os.environ.get("MINIHUB_MODULE_SDK_PATH", "").strip()
        if sdk:
            pack_py = Path(sdk).expanduser() / "pack_store_module.py"
    if not pack_py.is_file():
        pack_py = root / "docs" / "module_sdk" / "pack_store_module.py"
    if not pack_py.is_file():
        raise SystemExit("pack_store_module.py not found next to this script or under tools/")
    cmd = [sys.executable, str(pack_py), "pack", *module_ids]
    print(f"== pack {' '.join(module_ids)} ==")
    subprocess.check_call(cmd, cwd=root)


def resolve_plugin_dir(
    module_id: str,
    *,
    root: Path = ROOT,
    plugins_dirs: list[Path] | None = None,
    include_demos: bool = True,
) -> Path:
    for base in candidate_roots(root, plugins_dirs, include_demos):
        # Entry may be the plugin dir itself.
        if base.name == module_id and _is_buildable(base):
            return base
        cand = base / module_id
        if (cand / "module.json").is_file():
            info = json.loads((cand / "module.json").read_text(encoding="utf-8"))
            runtime = info.get("runtime") or "native"
            if runtime not in ("native", "hybrid"):
                raise ValueError(
                    f"{module_id}: runtime={runtime!r} — "
                    "standalone CMake builds native/hybrid .so only"
                )
            if not (cand / "CMakeLists.txt").is_file():
                raise FileNotFoundError(f"missing {cand / 'CMakeLists.txt'}")
            if runtime == "hybrid" and not (cand / "main.js").is_file():
                raise FileNotFoundError(f"hybrid module missing {cand / 'main.js'}")
            return cand
    raise FileNotFoundError(
        f"missing examples/plugins/{module_id}/module.json "
        f"(searched: {', '.join(str(b) for b in candidate_roots(root, plugins_dirs)) or 'MINIHUB_PLUGIN_DIRS unset'})"
    )


def discover_module_ids(
    *,
    root: Path = ROOT,
    plugins_dirs: list[Path] | None = None,
    include_demos: bool = True,
) -> list[str]:
    """All native/hybrid ids across examples/plugins + MINIHUB_PLUGIN_DIRS."""
    ids: list[str] = []
    seen: set[str] = set()
    for base in candidate_roots(root, plugins_dirs, include_demos):
        entries = [base] if _is_buildable(base) else sorted(p for p in base.iterdir() if p.is_dir())
        for d in entries:
            if _is_buildable(d) and d.name not in seen:
                seen.add(d.name)
                ids.append(d.name)
    return ids


def default_cc() -> str:
    return os.environ.get("MINIHUB_PLUGIN_CC", "riscv32-esp-elf-gcc")


def _resolve_sdk_path(root: Path = ROOT, sdk: Path | None = None) -> Path:
    """Unzipped module_sdk/ (standalone) or docs/module_sdk in a MiniHub tree.

    Standalone builds need only this directory — not an esp-minihub checkout.
    """
    if sdk is not None:
        path = sdk.expanduser().resolve()
    else:
        env = os.environ.get("MINIHUB_MODULE_SDK_PATH", "").strip()
        if env:
            path = Path(env).expanduser().resolve()
        elif (_here() / "minihub_plugin_standalone.cmake").is_file():
            path = _here()
        else:
            path = root / "docs" / "module_sdk"
    cmake = path / "minihub_plugin_standalone.cmake"
    hdr = path / "include" / "minihub_plugin_sdk.h"
    if not cmake.is_file() or not hdr.is_file():
        raise SystemExit(
            f"Plugin SDK incomplete under {path} "
            "(need minihub_plugin_standalone.cmake and include/minihub_plugin_sdk.h); "
            "unzip module_sdk.zip and set MINIHUB_MODULE_SDK_PATH"
        )
    return path


def run_standalone(
    module_id: str,
    *,
    root: Path = ROOT,
    plugins_dirs: list[Path] | None = None,
    cc: str | None = None,
    clean: bool = False,
    pack: bool = False,
    include_demos: bool = True,
    sdk: Path | None = None,
) -> None:
    plugin = resolve_plugin_dir(
        module_id, root=root, plugins_dirs=plugins_dirs, include_demos=include_demos
    )
    # Standalone outs stay under the plugin tree (gitignore: build/ any depth).
    build_dir = plugin / "build"
    compiler = cc or default_cc()
    if shutil.which(compiler) is None:
        raise SystemExit(f"compiler not on PATH: {compiler}")

    if clean and build_dir.exists():
        shutil.rmtree(build_dir)

    sdk = _resolve_sdk_path(root, sdk)
    cmake = shutil.which("cmake") or "cmake"
    configure = [
        cmake,
        "-S",
        str(plugin),
        "-B",
        str(build_dir),
        f"-DCMAKE_C_COMPILER={compiler}",
        # Avoid host flags (e.g. macOS -arch arm64) on the RISC-V cross gcc.
        "-DCMAKE_SYSTEM_NAME=Generic",
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        # Unzipped module_sdk/ (or docs/module_sdk in a MiniHub tree).
        f"-DMINIHUB_MODULE_SDK_PATH={sdk}",
        f"-DMINIHUB_SDK_INCLUDE={sdk / 'include'}",
    ]
    build = [cmake, "--build", str(build_dir)]
    if pack:
        build.extend(["--target", "pack"])

    env = os.environ.copy()
    # pack_store_module resolves by id via examples/plugins then
    # MINIHUB_PLUGIN_DIRS. Always put this plugin's parent first so
    # demos under examples/plugins pack even when CI already set the env to
    # a sibling Store repo that does not contain the demo id.
    parent = str(plugin.parent.resolve())
    existing = env.get("MINIHUB_PLUGIN_DIRS", "").strip()
    if existing:
        parts = [p for p in existing.replace(";", ":").split(":") if p.strip()]
        if parent not in parts:
            env["MINIHUB_PLUGIN_DIRS"] = parent + ":" + existing
    else:
        env["MINIHUB_PLUGIN_DIRS"] = parent

    subprocess.check_call(configure, cwd=root, env=env)
    subprocess.check_call(build, cwd=root, env=env)

    mhmd = build_dir / f"{module_id}.so"
    if not mhmd.is_file():
        raise SystemExit(f"expected MHMD missing: {mhmd}")
    print(f"OK: {mhmd} ({mhmd.stat().st_size} bytes)")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "module_id",
        nargs="*",
        help="plugin ids (examples/plugins/<id> or under MINIHUB_PLUGIN_DIRS); omit with --all",
    )
    p.add_argument("--all", action="store_true", help="build every native/hybrid plugin found")
    p.add_argument(
        "--no-demos",
        action="store_true",
        help="with --all: skip examples/plugins (build only MINIHUB_PLUGIN_DIRS / --plugins-dir)",
    )
    p.add_argument(
        "--plugins-dir",
        action="append",
        default=None,
        help="plugin root (repeatable); default: MINIHUB_PLUGIN_DIRS",
    )
    p.add_argument("--cc", default=None, help="C compiler (default riscv32-esp-elf-gcc)")
    p.add_argument(
        "--sdk",
        default=None,
        help="unzipped module_sdk/ (default: MINIHUB_MODULE_SDK_PATH or <repo>/docs/module_sdk)",
    )
    p.add_argument("--clean", action="store_true")
    p.add_argument(
        "--pack",
        action="store_true",
        help="after native builds: CMake 'pack' for L3, then pack L1/L2 from the same roots",
    )
    args = p.parse_args(argv)

    plugins_dirs = (
        [Path(x).expanduser().resolve() for x in args.plugins_dir]
        if args.plugins_dir
        else None
    )
    sdk = Path(args.sdk).expanduser().resolve() if args.sdk else None
    include_demos = not args.no_demos

    ids = list(args.module_id)
    if args.all or not ids:
        discovered = discover_module_ids(
            root=ROOT, plugins_dirs=plugins_dirs, include_demos=include_demos
        )
        js_pack = (
            discover_js_pack_ids(
                root=ROOT, plugins_dirs=plugins_dirs, include_demos=include_demos
            )
            if args.pack
            else []
        )
        if not discovered and not js_pack:
            raise SystemExit(
                "no plugins found — export MINIHUB_PLUGIN_DIRS or pass --plugins-dir"
            )
        ids = discovered

    for module_id in ids:
        print(f"== standalone {module_id} ==")
        run_standalone(
            module_id,
            root=ROOT,
            plugins_dirs=plugins_dirs,
            cc=args.cc,
            clean=args.clean,
            pack=args.pack,
            include_demos=include_demos,
            sdk=sdk,
        )

    if args.pack:
        # L1/L2 never go through CMake; pack any under the same search roots.
        # (Named L3 ids were already packed via CMake --target pack when listed.)
        sidecar = discover_js_pack_ids(
            root=ROOT, plugins_dirs=plugins_dirs, include_demos=include_demos
        )
        if sidecar:
            pack_store_ids(sidecar, root=ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

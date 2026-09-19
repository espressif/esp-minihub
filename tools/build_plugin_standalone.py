#!/usr/bin/env python3
"""Build an examples/plugins L3 demo without ESP-IDF (CMake + Plugin SDK)."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def resolve_plugin_dir(module_id: str, *, root: Path = ROOT) -> Path:
    d = root / "examples" / "plugins" / module_id
    if not d.is_dir() or not (d / "module.json").is_file():
        raise FileNotFoundError(f"missing examples/plugins/{module_id}/module.json")
    manifest = json.loads((d / "module.json").read_text(encoding="utf-8"))
    if manifest.get("runtime") != "native":
        raise ValueError(
            f"{module_id}: runtime={manifest.get('runtime')!r} — "
            "standalone CMake is L3 native only"
        )
    if not (d / "CMakeLists.txt").is_file():
        raise FileNotFoundError(f"missing {d / 'CMakeLists.txt'}")
    return d


def default_cc() -> str:
    return os.environ.get("MINIHUB_PLUGIN_CC", "riscv32-esp-elf-gcc")


def run_standalone(
    module_id: str,
    *,
    root: Path = ROOT,
    cc: str | None = None,
    clean: bool = False,
    pack: bool = False,
) -> None:
    plugin = resolve_plugin_dir(module_id, root=root)
    # Standalone outs stay under the plugin tree (gitignore: build/ any depth).
    build_dir = plugin / "build"
    compiler = cc or default_cc()
    if shutil.which(compiler) is None:
        raise SystemExit(f"compiler not on PATH: {compiler}")

    if clean and build_dir.exists():
        shutil.rmtree(build_dir)

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
    ]
    build = [cmake, "--build", str(build_dir)]
    if pack:
        build.extend(["--target", "pack"])

    env = os.environ.copy()
    env.setdefault("MINIHUB_APP", "minihub")
    env.setdefault("MINIHUB_PLUGIN_DIRS", str(root / "examples" / "plugins"))

    subprocess.check_call(configure, cwd=root, env=env)
    subprocess.check_call(build, cwd=root, env=env)

    mhmd = build_dir / f"{module_id}.so"
    if not mhmd.is_file():
        raise SystemExit(f"expected MHMD missing: {mhmd}")
    print(f"OK: {mhmd} ({mhmd.stat().st_size} bytes)")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("module_id", help="examples/plugins/<id> (L3 native)")
    p.add_argument("--cc", default=None, help="C compiler (default riscv32-esp-elf-gcc)")
    p.add_argument("--clean", action="store_true")
    p.add_argument("--pack", action="store_true", help="also build CMake target 'pack'")
    args = p.parse_args(argv)
    run_standalone(args.module_id, cc=args.cc, clean=args.clean, pack=args.pack)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Publish the full flash set into <deploy>/Firmwares/<chip>/<version>/.

That folder already holds the OTA app image and release_notes.txt. A flashing
tool wants the whole device written in one shot, so this copies every image
listed in the build's ``flash_args`` (bootloader, partition table, ota data,
storage, modstore, app, ...) alongside a merged.bin produced by esptool, plus
the app debug ELF (the ``.elf`` next to the app ``.bin`` referenced by
``app-flash_args``, e.g. minihub.elf):

  <deploy>/Firmwares/<chip>/<version>/
      <app>.bin
      release_notes.txt
      <every flash_args image, by basename>
      merged.bin
      <app>.elf
      flash_command.txt   # esptool write_flash command (offsets + flags from flash_args)

GET /api/firmware lists the app image only, not the flash parts. A leftover
Binaries/<chip>/<version>/ from an older deploy is removed.

Usage:
  python3 tools/gen_flash_binaries.py --deploy ~/workspace/minihub_store \
      --chip esp32s31 --version v1.2.3 --app minihub
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent  # tools/ lives directly under the repo root

# Reuse build_firmware.run_merge_bin (handles IDF env + esptool + flasher_args.json).
sys.path.insert(0, str(TOOLS))
from build_firmware import BUILD_DIR, set_app  # noqa: E402


def _parse_flash_args(path: Path) -> tuple[str, list[tuple[str, str]]]:
    """Parse build/flash_args.

    Returns ``(flags, [(offset, relpath), ...])``. Header lines start with ``-``
    and carry esptool flags; offset lines are ``0xNNNN rel/path``. The app
    ``app-flash_args`` uses the same format and lists only the app image.
    """
    flags = ""
    pairs: list[tuple[str, str]] = []
    if not path.is_file():
        return flags, pairs
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("-"):
            flags = s
            continue
        parts = s.split()
        if len(parts) == 2:
            off, rel = parts
            try:
                int(off, 0)
            except ValueError:
                continue
            pairs.append((off, rel))
    return flags, pairs


def _flash_command(build: Path, chip: str, app: str) -> str:
    """esptool write_flash commands. Individual partitions come from
    build/flash_args (faithful: every image + its flags); the one-shot merged
    image is listed too."""
    flags, pairs = _parse_flash_args(build / "flash_args")
    lines = [f"# Flash full MiniHub firmware to {chip}:"]
    if pairs:
        parts = [f"  {off} {Path(rel).name}" for off, rel in sorted(pairs, key=lambda kv: int(kv[0], 0))]
        head = f"esptool.py --chip {chip}"
        if flags:
            head += f" {flags}"
        head += " write_flash \\"
        lines.append(head)
        lines.append(" \\\n".join(parts))
    lines.append("")
    lines.append(f"# Or flash the one-shot merged image:")
    lines.append(f"esptool.py --chip {chip} write_flash 0x0 merged.bin")
    lines.append("")
    return "\n".join(lines)


def _remove_legacy_binaries(deploy: Path, chip: str, version: str) -> None:
    """Drop Binaries/<chip>/<version> left by older deploys."""
    root = deploy / "Binaries"
    if not root.is_dir():
        return
    for name in {chip, chip.upper()}:
        legacy = root / name / version
        if legacy.is_dir():
            shutil.rmtree(legacy)
        parent = root / name
        if parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
    if not any(root.iterdir()):
        root.rmdir()


def _ensure_merged(build: Path, app: str) -> bool:
    """Make sure build/<app>_merged.bin exists; generate it if missing."""
    merged = build / f"{app}_merged.bin"
    if merged.is_file():
        return True
    # build_firmware.py merge sources the IDF env and runs esptool merge-bin.
    # NOTE: the -app flag must precede the `merge` verb (parse_argv only consumes
    # -app before any idf.py argument / verb).
    cmd = [sys.executable, str(TOOLS / "build_firmware.py"), "-app", app, "merge"]
    rc = subprocess.run(cmd, cwd=str(ROOT)).returncode
    return rc == 0 and merged.is_file()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--deploy", required=True, type=Path, help="deploy root")
    ap.add_argument("--chip", default="esp32s31", help="chip folder, e.g. esp32s31")
    ap.add_argument("--version", required=True, help="version folder name")
    ap.add_argument("--app", default=None, help="application folder (default: MINIHUB_APP or minihub)")
    args = ap.parse_args(argv)

    app = (args.app or os.environ.get("MINIHUB_APP") or "minihub").strip() or "minihub"
    set_app(app)
    build = BUILD_DIR

    if not (build / "flasher_args.json").is_file():
        print(
            f"gen_flash_binaries: no build at {build} (run build_firmware.py first)",
            file=sys.stderr,
        )
        return 1

    chip = args.chip.strip().lower()
    deploy = args.deploy.expanduser()
    bin_dir = deploy / "Firmwares" / chip / args.version
    bin_dir.mkdir(parents=True, exist_ok=True)
    _remove_legacy_binaries(deploy, chip, args.version)

    merged = build / f"{app}_merged.bin"

    # 1) Every image listed in build/flash_args (bootloader, partition table,
    #    ota_data_initial, storage, modstore, app, ...), copied by basename.
    flash_args = build / "flash_args"
    _, pairs = _parse_flash_args(flash_args)
    if not pairs:
        # Fallback for very old builds that lack flash_args entirely.
        pairs = [
            ("0x2000", "bootloader/bootloader.bin"),
            ("0x8000", "partition_table/partition-table.bin"),
            ("0x20000", f"{app}.bin"),
        ]
    copied: list[str] = []
    for off, rel in sorted(pairs, key=lambda kv: int(kv[0], 0)):
        base = Path(rel).name
        src = build / rel
        if src.is_file():
            shutil.copy2(src, bin_dir / base)
            copied.append(base)
        else:
            print(f"gen_flash_binaries: missing {src}", file=sys.stderr)

    # 2) App debug ELF: the .elf next to the app .bin in app-flash_args.
    _, app_pairs = _parse_flash_args(build / "app-flash_args")
    for off, rel in app_pairs:
        elf = Path(rel).with_suffix(".elf")
        src = build / elf
        if src.is_file():
            shutil.copy2(src, bin_dir / elf.name)
            copied.append(elf.name)
        else:
            print(f"gen_flash_binaries: missing {src}", file=sys.stderr)

    # 3) One-shot merged image (esptool merge of every partition).
    if _ensure_merged(build, app):
        shutil.copy2(merged, bin_dir / "merged.bin")
        copied.append("merged.bin")
    else:
        print(
            "gen_flash_binaries: merged.bin NOT generated (esptool unavailable / no "
            "build). Run `build_firmware.py build merge` to produce it.",
            file=sys.stderr,
        )

    (bin_dir / "flash_command.txt").write_text(
        _flash_command(build, chip, app), encoding="utf-8"
    )

    if not copied:
        print(f"gen_flash_binaries: nothing to copy from {build}", file=sys.stderr)
        return 1
    print(f"gen_flash_binaries: wrote {bin_dir} [{', '.join(copied)}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

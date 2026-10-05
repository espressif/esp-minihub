#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Copy a firmware .bin and a release note into Firmwares/<chip>/<version>/.

The version folder has no json. store_server.py derives version, chip,
sha256, size, and release_note when it walks the tree. The file written
there is release_notes.txt. Pass the text with --note, or a file such as
release_notes.txt with --notes.

  python3 tools/publish_firmware.py \\
      --bin application/minihub/build/minihub.bin \\
      --chip ESP32S31 --version v0.1.0 \\
      --notes release_notes.txt \\
      --deploy ~/workspace/minihub_website_internal
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import resolve_deploy_dir  # noqa: E402


def release_note_text(*, note: str | None, notes: Path | None) -> str:
    """Return the note body. ``note`` is literal text; ``notes`` is a file."""
    if (note is None) == (notes is None):
        raise ValueError(
            "pass exactly one of --note (text) or --notes (a file such as release_notes.txt)"
        )
    if notes is not None:
        if not notes.is_file():
            raise FileNotFoundError(
                f"--notes must be an existing file such as release_notes.txt (got {notes})"
            )
        text = notes.read_text(encoding="utf-8")
    else:
        text = note or ""
    if text and not text.endswith("\n"):
        text += "\n"
    return text


def publish_firmware(
    blob: Path,
    *,
    chip: str,
    version: str,
    note: str,
    deploy: Path,
    name: str | None = None,
) -> Path:
    if not blob.is_file():
        raise FileNotFoundError(f"missing firmware bin {blob}")
    chip_name = chip.strip().upper()
    version_name = version.strip()
    if not chip_name or not version_name or "/" in chip_name or "/" in version_name:
        raise ValueError("chip and version must be single path segments")
    if ".." in chip_name or ".." in version_name:
        raise ValueError("chip and version must not contain '..'")
    dest_dir = deploy / "Firmwares" / chip_name / version_name
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest_name = name.strip() if name else blob.name
    if not dest_name.endswith(".bin") or "/" in dest_name or dest_name.startswith("."):
        raise ValueError("firmware file name must be a single *.bin segment")
    dest = dest_dir / dest_name
    shutil.copyfile(blob, dest)
    text = note
    if text and not text.endswith("\n"):
        text += "\n"
    (dest_dir / "release_notes.txt").write_text(text, encoding="utf-8")
    return dest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True, help="firmware .bin to publish")
    parser.add_argument("--chip", required=True, help="chip folder, e.g. ESP32S31")
    parser.add_argument("--version", required=True, help="version folder name")
    parser.add_argument(
        "--note",
        default=None,
        help="release note text (written as release_notes.txt). Mutually exclusive with --notes",
    )
    parser.add_argument(
        "--notes",
        type=Path,
        default=None,
        help="release note file to read, for example release_notes.txt. "
        "Written into the version folder as release_notes.txt. Mutually exclusive with --note",
    )
    parser.add_argument("--name", default=None, help="destination bin file name (default: source name)")
    parser.add_argument(
        "--deploy",
        type=Path,
        default=None,
        help="deploy root (default: MINIHUB_PLUGINS_DEPLOY_DIR or ~/workspace/minihub_store)",
    )
    args = parser.parse_args(argv)
    try:
        text = release_note_text(note=args.note, notes=args.notes)
    except (ValueError, FileNotFoundError) as exc:
        parser.error(str(exc))
    deploy = resolve_deploy_dir(args.deploy)
    dest = publish_firmware(
        args.bin,
        chip=args.chip,
        version=args.version,
        note=text,
        deploy=deploy,
        name=args.name,
    )
    print(f"publish_firmware: {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

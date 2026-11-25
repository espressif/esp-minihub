#!/usr/bin/env python3
"""Stage LCD UI assets into the littlefs image, minifying JSON at build time.

The source ``ui/**/*.json`` files keep their hand-maintained formatting because
``tools/lcd_ui_editor/server.py`` writes them back byte-preservingly. Runtime
only needs the data, so the image copy is minified -- the same approach as
``tools/minify_web_assets.py`` uses for the Web console.

``ui/i18n/strings.json`` is the build-time dictionary source (it generates the
language constants and ``lcd_strings.gen.h``). ``root.json`` never references
it, so it is not staged into the device image at all.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil


def _canonical_version(raw: str) -> str:
    v = (raw or "").strip()
    if len(v) >= 2 and (v[0] == "v" or v[0] == "V") and v[1].isdigit():
        return v[1:]
    return v


def _stage(src: pathlib.Path, dst: pathlib.Path, quiet: bool) -> int:
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    minified = 0
    for path in sorted(src.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(src)
        # Build stamps / editor junk must not ship in LCD_UI ZIPs or deploy trees.
        if any(part == ".stamp" or part.endswith(".stamp") or part == ".DS_Store"
               for part in rel.parts):
            continue
        if rel.as_posix() == "i18n/strings.json":
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix == ".json":
            data = json.loads(path.read_text(encoding="utf-8"))
            out.write_text(
                json.dumps(data, ensure_ascii=False, separators=(",", ":")),
                encoding="utf-8",
            )
            minified += 1
        else:
            shutil.copy2(path, out)
    if not quiet:
        print(f"minified {minified} LCD UI JSON assets -> {dst}")
    return minified


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", required=True, help="components/minihub_lcd/ui")
    ap.add_argument("--dst", help="littlefs image ui/ directory")
    ap.add_argument("--deploy-root", help="write LCD_UI/<version>/ under this deploy root")
    ap.add_argument(
        "--ui-version",
        default="",
        help="CONFIG_MINIHUB_LCD_UI_VERSION (LCD_UI/v<ver>/ + version.txt)",
    )
    ap.add_argument("--notes", help="optional release_notes.txt to copy into the bundle")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)

    src = pathlib.Path(args.src)
    if not args.dst and not args.deploy_root:
        ap.error("need --dst and/or --deploy-root")

    canon = _canonical_version(args.ui_version) if args.ui_version else ""
    folder = f"v{canon}" if canon else ""

    if args.dst:
        dst = pathlib.Path(args.dst)
        _stage(src, dst, args.quiet)
        if canon:
            (dst / "version.txt").write_text(canon + "\n", encoding="utf-8")

    if args.deploy_root:
        if not canon:
            ap.error("--deploy-root requires --ui-version")
        deploy = pathlib.Path(args.deploy_root)
        out = deploy / "LCD_UI" / folder
        _stage(src, out, args.quiet)
        (out / "version.txt").write_text(canon + "\n", encoding="utf-8")
        if args.notes:
            note = pathlib.Path(args.notes)
            if note.is_file():
                shutil.copy2(note, out / "release_notes.txt")
        if not args.quiet:
            print(f"published LCD UI -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

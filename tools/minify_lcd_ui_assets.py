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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True, help='components/minihub_lcd/ui')
    ap.add_argument('--dst', required=True, help='littlefs image ui/ directory')
    ap.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()

    src = pathlib.Path(args.src)
    dst = pathlib.Path(args.dst)
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)

    minified = 0
    for path in sorted(src.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(src)
        if rel.as_posix() == 'i18n/strings.json':
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix == '.json':
            data = json.loads(path.read_text(encoding='utf-8'))
            out.write_text(json.dumps(data, ensure_ascii=False,
                                      separators=(',', ':')), encoding='utf-8')
            minified += 1
        else:
            shutil.copy2(path, out)
    if not args.quiet:
        print(f'minified {minified} LCD UI JSON assets -> {dst}')


if __name__ == '__main__':
    main()

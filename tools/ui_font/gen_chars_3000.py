#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""GB2312 level-1 Hanzi (常用汉字区) for minihub_sc.otf subset."""
from __future__ import annotations

import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "tools" / "ui_font" / "data" / "chars_3000.txt"
DEFAULT_COUNT = 3000


def gb2312_level1_chars(limit: int = DEFAULT_COUNT) -> str:
    """First `limit` characters from GB2312 level-1 Hanzi block (qu/wei 16–55)."""
    chars: list[str] = []
    for qu in range(16, 56):
        for wei in range(1, 95):
            try:
                ch = bytes([0xA0 + qu, 0xA0 + wei]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                chars.append(ch)
                if len(chars) >= limit:
                    return "".join(chars)
    return "".join(chars)


def main() -> int:
    parser = argparse.ArgumentParser(description="Write GB2312 level-1 Hanzi subset list")
    parser.add_argument("-n", "--count", type=int, default=DEFAULT_COUNT, help="Hanzi count (default 3000)")
    parser.add_argument("-o", "--output", type=pathlib.Path, default=DEFAULT_OUT, help="Output .txt path")
    parser.add_argument("--stdout", action="store_true", help="Print to stdout instead of writing a file")
    args = parser.parse_args()

    text = gb2312_level1_chars(args.count)
    if args.stdout:
        sys.stdout.write(text)
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text + "\n", encoding="utf-8")
    print(f"Wrote {args.output} ({len(text)} chars, {args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

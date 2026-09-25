#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Strip comments / excess blank lines from Host Web console assets.

Conservative: removes block comments, full-line //, HTML comments, and
extra blank lines. Does not touch inline // (avoids http:// and regex).

  python3 tools/minify_web_assets.py --src components/minihub_web/web --dst /tmp/web
  python3 tools/minify_web_assets.py --src components/minihub_web/web --in-place
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


def _strip_block_comments(src: str) -> str:
    """Remove /* … */ outside quotes / template literals."""
    out: list[str] = []
    i = 0
    n = len(src)
    in_sq = in_dq = in_tq = False

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if in_tq:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == "`":
                in_tq = False
            i += 1
            continue

        if in_sq:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == "'":
                in_sq = False
            i += 1
            continue

        if in_dq:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == '"':
                in_dq = False
            i += 1
            continue

        if c == "'":
            in_sq = True
            out.append(c)
            i += 1
            continue
        if c == '"':
            in_dq = True
            out.append(c)
            i += 1
            continue
        if c == "`":
            in_tq = True
            out.append(c)
            i += 1
            continue

        if c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
            # Keep a single space so tokens do not glue together.
            if out and out[-1] not in " \t\n":
                out.append(" ")
            continue

        out.append(c)
        i += 1

    return "".join(out)


def _strip_full_line_slash_comments(src: str) -> str:
    """Drop lines whose first non-space content is // (not http://)."""
    out: list[str] = []
    for ln in src.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        s = ln.lstrip()
        if s.startswith("//"):
            continue
        out.append(ln)
    return "\n".join(out)


def _strip_html_comments(src: str) -> str:
    out: list[str] = []
    i = 0
    n = len(src)
    while i < n:
        if src.startswith("<!--", i):
            end = src.find("-->", i + 4)
            if end < 0:
                break
            i = end + 3
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _collapse_blank_lines(src: str) -> str:
    lines = src.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out: list[str] = []
    blank = 0
    for ln in lines:
        if ln.strip() == "":
            blank += 1
            if blank <= 1:
                out.append("")
            continue
        blank = 0
        out.append(ln.rstrip())
    return "\n".join(out).strip() + "\n"


def minify_text(path: Path, text: str) -> str:
    suf = path.suffix.lower()
    if suf == ".js":
        text = _strip_block_comments(text)
        text = _strip_full_line_slash_comments(text)
    elif suf == ".css":
        text = _strip_block_comments(text)
    elif suf in {".html", ".htm"}:
        text = _strip_html_comments(text)
        # Also strip block comments inside <script> / <style>.
        lower = text.lower()
        pieces: list[str] = []
        i = 0
        while i < len(text):
            if lower.startswith("<script", i):
                tag_end = text.find(">", i)
                close = lower.find("</script>", tag_end if tag_end >= 0 else i)
                if tag_end < 0 or close < 0:
                    pieces.append(text[i:])
                    break
                pieces.append(text[i : tag_end + 1])
                body = text[tag_end + 1 : close]
                body = _strip_block_comments(body)
                body = _strip_full_line_slash_comments(body)
                pieces.append(body)
                pieces.append(text[close : close + len("</script>")])
                i = close + len("</script>")
                continue
            if lower.startswith("<style", i):
                tag_end = text.find(">", i)
                close = lower.find("</style>", tag_end if tag_end >= 0 else i)
                if tag_end < 0 or close < 0:
                    pieces.append(text[i:])
                    break
                pieces.append(text[i : tag_end + 1])
                body = _strip_block_comments(text[tag_end + 1 : close])
                pieces.append(body)
                pieces.append(text[close : close + len("</style>")])
                i = close + len("</style>")
                continue
            pieces.append(text[i])
            i += 1
        text = "".join(pieces)
    else:
        return text
    return _collapse_blank_lines(text)


def process_tree(src: Path, dst: Path | None, *, in_place: bool) -> tuple[int, int, int]:
    if not src.is_dir():
        raise SystemExit(f"not a directory: {src}")
    files = 0
    bin_in = 0
    bout = 0
    for path in sorted(src.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(src)
        raw = path.read_bytes()
        bin_in += len(raw)
        if path.suffix.lower() in {".js", ".css", ".html", ".htm"}:
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                out_bytes = raw
            else:
                out_bytes = minify_text(path, text).encode("utf-8")
        else:
            out_bytes = raw
        bout += len(out_bytes)
        files += 1
        if in_place:
            if out_bytes != raw:
                path.write_bytes(out_bytes)
        else:
            assert dst is not None
            target = dst / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(out_bytes)
    return files, bin_in, bout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", required=True, type=Path)
    ap.add_argument("--dst", type=Path)
    ap.add_argument("--in-place", action="store_true")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()
    src = args.src.resolve()
    if args.in_place:
        if args.dst:
            raise SystemExit("--in-place and --dst are mutually exclusive")
        n, a, b = process_tree(src, None, in_place=True)
    else:
        if not args.dst:
            raise SystemExit("need --dst or --in-place")
        dst = args.dst.resolve()
        if dst.exists():
            shutil.rmtree(dst)
        dst.mkdir(parents=True, exist_ok=True)
        n, a, b = process_tree(src, dst, in_place=False)
    if not args.quiet:
        saved = a - b
        pct = (100.0 * saved / a) if a else 0.0
        print(f"minify_web_assets: {n} files {a} → {b} bytes (−{saved}, {pct:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

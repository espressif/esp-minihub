#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Serve Programming Guide HTML for local preview (default 127.0.0.1:8765).

Expects ``./tools/build_docs.sh`` output::

    docs/_build/en/esp32s31/html/
    docs/_build/zh_CN/esp32s31/html/

Usage::

    python3 tools/preview_docs.py
    python3 tools/preview_docs.py --port 8000 --open
    python3 tools/preview_docs.py --build-dir ~/website/MiniHub_docs --host 0.0.0.0
"""

from __future__ import annotations

import argparse
import mimetypes
import sys
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "docs" / "_build"
TARGET = "esp32s31"

LANGS = ("en", "zh_CN")


def html_dir(build_root: Path, lang: str) -> Path:
    return build_root / lang / TARGET / "html"


def resolve_lang_dirs(build_root: Path) -> dict[str, Path]:
    found: dict[str, Path] = {}
    missing: list[str] = []
    for lang in LANGS:
        path = html_dir(build_root, lang)
        index = path / "index.html"
        if index.is_file():
            found[lang] = path.resolve()
        else:
            missing.append(str(path))
    if not found:
        print("No built HTML under:", build_root, file=sys.stderr)
        print("Expected: <build-dir>/en/esp32s31/html/index.html", file=sys.stderr)
        for p in missing:
            print(f"  missing: {p}/index.html", file=sys.stderr)
        sys.exit(1)
    for lang in missing:
        print(f"warning: skip (not built): {lang}", file=sys.stderr)
    return found


def safe_join(base: Path, rel: str) -> Path | None:
    rel = unquote(rel).lstrip("/")
    if not rel:
        candidate = (base / "index.html").resolve()
    else:
        candidate = (base / rel).resolve()
    try:
        candidate.relative_to(base)
    except ValueError:
        return None
    if candidate.is_dir():
        candidate = (candidate / "index.html").resolve()
        try:
            candidate.relative_to(base)
        except ValueError:
            return None
    return candidate if candidate.is_file() else None


def make_handler(roots: dict[str, Path], default_lang: str):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args) -> None:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

        def do_HEAD(self) -> None:
            self._handle(send_body=False)

        def do_GET(self) -> None:
            self._handle(send_body=True)

        def _handle(self, send_body: bool) -> None:
            parsed = urlparse(self.path)
            path = parsed.path or "/"

            if path in ("/", "/index.html"):
                self.send_response(302)
                self.send_header("Location", f"/{default_lang}/")
                self.end_headers()
                return

            lang = None
            rest = path.lstrip("/")
            for name in LANGS:
                prefix = name + "/"
                if rest == name or rest.startswith(prefix):
                    lang = name
                    rest = rest[len(name) :].lstrip("/")
                    break
            if lang is None or lang not in roots:
                self.send_error(404, "Not found")
                return

            file_path = safe_join(roots[lang], rest)
            if file_path is None:
                self.send_error(404, "Not found")
                return

            ctype, _ = mimetypes.guess_type(str(file_path))
            data = file_path.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", ctype or "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            if send_body:
                self.wfile.write(data)

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description="Preview MiniHub Programming Guide HTML")
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD,
        help="esp-docs output root (contains en/esp32s31/html). "
        "Default: <repo>/docs/_build",
    )
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--lang", choices=LANGS, default="zh_CN", help="redirect / to this language")
    parser.add_argument("--open", action="store_true", help="open default browser")
    args = parser.parse_args()

    roots = resolve_lang_dirs(args.build_dir.expanduser().resolve())
    if args.lang not in roots:
        args.lang = next(iter(roots))

    handler = make_handler(roots, args.lang)
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    url = f"http://{args.host}:{args.port}/{args.lang}/"
    print("Preview (built HTML only; rebuild after RST/header changes):")
    for lang in LANGS:
        if lang in roots:
            print(f"  http://{args.host}:{args.port}/{lang}/")
    print(f"Listening on {args.host}:{args.port}  (Ctrl+C to stop)")
    if args.open:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

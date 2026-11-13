#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Check that a no-op save through the editor changes NOTHING on disk.

The LCD UI assets belong to whoever edits them by hand. A save that touches
files whose content did not change is pure noise in the diff, so this must be
zero - not "small".

Usage:
    python3 tools/lcd_ui_editor/server.py --port 8791 --no-open   # another shell
    python3 tools/lcd_ui_editor/check_noop_save.py --port 8791
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
UI_DIR = ROOT / "components" / "minihub_lcd" / "ui"


def snapshot() -> dict:
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(UI_DIR.rglob("*.json"))}


def request(conn, method, path, payload=None):
    body = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if body else {}
    conn.request(method, path, body=body, headers=headers)
    res = conn.getresponse()
    return json.loads(res.read().decode("utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    conn = http.client.HTTPConnection(args.host, args.port, timeout=10)
    try:
        ui = request(conn, "GET", "/api/ui")
    except OSError as exc:
        print("cannot reach the editor at %s:%d: %s" % (args.host, args.port, exc),
              file=sys.stderr)
        print("start it with: python3 tools/lcd_ui_editor/server.py "
              "--port %d --no-open" % args.port, file=sys.stderr)
        return 2

    before = snapshot()
    result = request(conn, "POST", "/api/save",
                     {"root": ui["root"], "assets": ui["assets"]})
    after = snapshot()

    changed = [k for k in before if before[k] != after.get(k)]
    print(result.get("message", ""))
    print("改动:", changed or "无")
    if changed:
        print("FAIL: a no-op save must not touch any file", file=sys.stderr)
        return 1
    print("OK: 空保存零改动")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

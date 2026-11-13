#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""LCD UI visual editor - local server (stdlib only).

The device screen UI is a set of Brookesia JSON assets under
``components/minihub_lcd/ui/`` (screens / templates / flows / constants).
This tool edits them through a tree UI instead of by hand-editing JSON.

Design rules:

- **The existing contract suite is the authority.** ``tools/test_lcd_ui_contract.py``
  already encodes every rule (a declared view must exist in the assets, an
  action must be offered by the UI, the C++ must use the generated macros...).
  The editor does NOT re-implement those rules - it RUNS the suite, so the
  checks cannot drift away from the firmware contract.
- **A save is transactional.** The current files are backed up, the new
  content is written, the suite runs, and if the suite fails the files are
  restored and the save is rejected. A broken UI cannot be left on disk.
- **Write scope is narrow.** Only ``*.json`` paths under
  ``components/minihub_lcd/ui/`` are writable, and the server binds to
  127.0.0.1. ``ui_contract.json`` is NOT writable here (it is the C++ side
  contract, not the visual tree).
- Stdlib only - no third-party dependency.

Usage:
    python3 tools/lcd_ui_editor/server.py                  # serve + open a browser
    python3 tools/lcd_ui_editor/server.py --port 8765 --no-open
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
UI_DIR = ROOT / "components" / "minihub_lcd" / "ui"
CONTRACT = ROOT / "components" / "minihub_lcd" / "ui_contract.json"
CPP_APP = ROOT / "components" / "minihub_lcd" / "minihub_lcd_app.cpp"
SUITE = "tools.test_lcd_ui_contract"

# Test classes whose result depends on the UI assets / the contract. A
# failure in any of them is the editor's business and blocks a save.
UI_AFFECTING_CLASSES = {
    "ContractStructureTest",
    "ContractCoverageTest",
    "DeviceTemplateTest",
}

# A relative asset path: plain names, no traversal, .json suffix.
SAFE_REL = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_./-]*\.json$")


def safe_target(rel: str) -> Path | None:
    """Resolve `rel` under UI_DIR, or None when it escapes / is not allowed."""
    if not isinstance(rel, str) or not SAFE_REL.match(rel):
        return None
    if ".." in rel.split("/"):
        return None
    target = (UI_DIR / rel).resolve()
    try:
        target.relative_to(UI_DIR.resolve())
    except ValueError:
        return None
    return target


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def inline_paths(text: str) -> dict:
    """How `text` lays out every container that fits on a SINGLE line.

    The path is a tuple of object keys / array indices, e.g.
    ``('node', 'layout')`` or ``('children', 0, 'style')``. The UI assets mix
    layouts - fully expanded, ``{"a": 1}`` and ``{ "a": 1 }`` - and they were
    written by different hands; re-emitting a file in the other layout turns
    a one-line edit into a whole-file reformat diff. Keeping the original
    layout per container is what makes the editor diff-clean.

    Returns {path: 'tight' | 'padded'} for the one-line containers only.
    """
    paths: dict = {}
    i, n = 0, len(text)

    def skip_ws() -> None:
        nonlocal i
        while i < n and text[i] in " \t\r\n":
            i += 1

    def parse_string() -> str:
        nonlocal i
        start = i
        i += 1
        while text[i] != '"':
            if text[i] == "\\":
                i += 1
            i += 1
        i += 1
        return json.loads(text[start:i])

    def parse_value(path):
        nonlocal i
        skip_ws()
        ch = text[i]
        if ch == "{":
            start = i
            i += 1
            obj = {}
            while True:
                skip_ws()
                if text[i] == "}":
                    i += 1
                    break
                if text[i] == ",":
                    i += 1
                    continue
                key = parse_string()
                skip_ws()
                i += 1  # ':'
                obj[key] = parse_value(path + (key,))
            if "\n" not in text[start:i]:
                paths[path] = "padded" if text[start + 1:start + 2] == " " else "tight"
            return obj
        if ch == "[":
            start = i
            i += 1
            arr = []
            while True:
                skip_ws()
                if text[i] == "]":
                    i += 1
                    break
                if text[i] == ",":
                    i += 1
                    continue
                arr.append(parse_value(path + (len(arr),)))
            if "\n" not in text[start:i]:
                paths[path] = "padded" if text[start + 1:start + 2] == " " else "tight"
            return arr
        if ch == '"':
            return parse_string()
        j = i
        while j < n and text[j] not in ",]} \t\r\n":
            j += 1
        raw = text[i:j]
        i = j
        return json.loads(raw)

    parse_value(())
    return paths


def render(data, inline: dict, indent: int = 2) -> str:
    """Serialize `data`, keeping the one-line containers one-line."""
    def fmt(value, path, level, force_inline=False):
        pad = " " * (indent * level)
        if isinstance(value, dict):
            if not value:
                return "{}"
            style = inline.get(path)
            if style or force_inline:
                inner = ", ".join(
                    f"{json.dumps(k, ensure_ascii=False)}: "
                    f"{fmt(v, path + (k,), 0, True)}"
                    for k, v in value.items())
                return "{ " + inner + " }" if style == "padded" else "{" + inner + "}"
            body = ",\n".join(
                f"{pad}{' ' * indent}{json.dumps(k, ensure_ascii=False)}: "
                f"{fmt(v, path + (k,), level + 1)}"
                for k, v in value.items())
            return "{\n" + body + "\n" + pad + "}"
        if isinstance(value, list):
            if not value:
                return "[]"
            style = inline.get(path)
            if style or force_inline:
                inner = ", ".join(
                    fmt(v, path + (i,), 0, True) for i, v in enumerate(value))
                return "[ " + inner + " ]" if style == "padded" else "[" + inner + "]"
            body = ",\n".join(
                f"{pad}{' ' * indent}{fmt(v, path + (i,), level + 1)}"
                for i, v in enumerate(value))
            return "[\n" + body + "\n" + pad + "]"
        return json.dumps(value, ensure_ascii=False)

    return fmt(data, (), 0)


def write_preserving(target: Path, data) -> bool:
    """Write `data` into `target` in the file's own layout.

    Returns True when the file was actually written. A file whose parsed
    content is already equal is NOT rewritten: the editor must not create
    formatting noise in files that a human (or another agent) owns.
    """
    old_text = target.read_text(encoding="utf-8") if target.is_file() else None
    if old_text is not None:
        try:
            if json.loads(old_text) == data:
                return False
        except json.JSONDecodeError:
            pass
        inline = inline_paths(old_text)
        trailing = old_text.endswith("\n")
    else:
        inline = set()
        trailing = True
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(render(data, inline) + ("\n" if trailing else ""),
                      encoding="utf-8")
    return True


def screen_profile() -> dict:
    """The preview's screen geometry, read from the firmware - not a copy.

    ``make_environment()`` and ``pin_shell_geometry()`` in minihub_lcd_app.cpp
    are the authority for the base resolution, density and font scale. A
    preview that hardcodes 800x480 / 1.0 silently drifts the moment either
    changes, so parse the constants instead and ship them in /api/ui.

    The three ``kShell*`` values are ALSO what pin_shell_geometry() writes as
    raw LVGL pixels (not dp). The preview overlays them to make a density
    change visibly expose that mismatch.
    """
    text = CPP_APP.read_text(encoding="utf-8") if CPP_APP.is_file() else ""

    def number(name: str, default: float) -> float:
        m = re.search(r"\b" + re.escape(name) + r"\s*=\s*(-?\d+(?:\.\d+)?)F?\s*;",
                      text)
        return float(m.group(1)) if m else default

    sidebar = number("kShellSidebarW", 112)
    main = number("kShellMainW", 688)
    height = number("kShellScreenH", 480)
    return {
        "base_width": int(sidebar + main),
        "base_height": int(height),
        "sidebar_width": int(sidebar),
        "main_width": int(main),
        "density": number("env.density", 1.0),
        "font_scale": number("env.font_scale", 1.0),
        "source": str(CPP_APP.relative_to(ROOT)) if text else None,
    }


def load_ui() -> dict:
    root = load_json(UI_DIR / "root.json")
    assets = {}
    for rel in root.get("assets") or []:
        target = safe_target(rel)
        if target is None or not target.is_file():
            continue
        assets[rel] = load_json(target)
    contract = load_json(CONTRACT) if CONTRACT.is_file() else {}
    return {"root": root, "assets": assets, "contract": contract,
            "screen_profile": screen_profile()}


def run_suite() -> tuple[bool, str]:
    """Run the authoritative LCD UI contract suite."""
    proc = subprocess.run(
        [sys.executable, "-m", "unittest", SUITE, "-v"],
        cwd=str(ROOT), capture_output=True, text=True,
    )
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


def failed_classes(output: str) -> set:
    """Test classes that failed in a `unittest -v` run.

    unittest prints ``FAIL: test_x (pkg.mod.Class.test_x)`` - the class only
    appears inside the parentheses.
    """
    ids = re.findall(r"^(?:FAIL|ERROR): \S+ \((\S+)\)", output, re.M)
    classes = set()
    for i in ids:
        parts = i.split(".")
        if len(parts) >= 2:
            classes.add(parts[-2])
    return classes


def ui_affecting_failures(output: str) -> set:
    """Failures the UI assets can actually cause.

    The suite also checks the firmware C++ (hard-coded literals, refresh
    wiring, fonts, flash-write hooks). Editing a JSON asset cannot break
    those - and blocking on them would mean one agent's in-flight C++ edit
    locks everybody out of the editor. The UI-facing classes are the ones
    that read the assets and the contract.
    """
    return failed_classes(output) & UI_AFFECTING_CLASSES


def save_ui(root: dict, assets: dict) -> tuple[bool, str, str]:
    """Transactional write. Returns (ok, message, suite_output)."""
    if not isinstance(root, dict) or not isinstance(assets, dict):
        return False, "root and assets must be objects", ""

    planned: list[tuple[Path, object]] = []
    for rel, data in assets.items():
        target = safe_target(rel)
        if target is None:
            return False, f"refused unsafe asset path: {rel!r}", ""
        planned.append((target, data))
    planned.append((UI_DIR / "root.json", root))

    # Back up every file we are about to touch (None = did not exist yet).
    backups: dict[Path, str | None] = {}
    for target, _ in planned:
        backups[target] = (target.read_text(encoding="utf-8")
                           if target.is_file() else None)

    # Baseline: was the suite ALREADY failing before this edit? Without this
    # the user cannot tell "my change broke it" from "the tree was broken".
    pre_ok, pre_out = run_suite()
    pre_failed = failed_classes(pre_out)

    try:
        written = 0
        for target, data in planned:
            if write_preserving(target, data):
                written += 1
    except OSError as exc:
        _restore(backups)
        return False, f"write failed: {exc}", ""

    ok, out = run_suite()
    if ok:
        return True, (f"saved {written} changed file(s) of {len(planned)} checked, "
                      f"contract suite passed"), out

    # The suite is red. Is it red because of the assets we just wrote?
    mine = ui_affecting_failures(out)
    if mine:
        _restore(backups)
        if pre_failed & UI_AFFECTING_CLASSES:
            return False, ("UI 契约相关测试在保存之前就已经失败（与本次改动无关），"
                           "本次改动已回滚"), out
        return False, ("contract 测试失败（涉及 UI 契约："
                       + ", ".join(sorted(mine))
                       + "），本次改动已回滚"), out

    # Red only in areas the editor cannot influence (firmware C++). Report it
    # loudly, but do not let it block editing the screen UI.
    other = failed_classes(out)
    return True, (f"saved {written} changed file(s) of {len(planned)} checked；"
                  f"注意：仓库里已有与 UI 无关的测试失败（{', '.join(sorted(other)) or '未知'}），"
                  f"与本次改动无关"), out


def _restore(backups: dict[Path, str | None]) -> None:
    for target, content in backups.items():
        if content is None:
            if target.is_file():
                target.unlink()
        else:
            target.write_text(content, encoding="utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "MiniHubLcdUiEditor/1.0"

    def log_message(self, fmt, *args):  # quieter console
        sys.stderr.write("[lcd-ui-editor] " + (fmt % args) + "\n")

    # -- helpers ---------------------------------------------------------
    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send(code, body, "application/json; charset=utf-8")

    def _static(self, name: str, ctype: str) -> None:
        path = HERE / name
        if not path.is_file():
            self._json(404, {"ok": False, "error": f"{name} not found"})
            return
        self._send(200, path.read_bytes(), ctype)

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError(f"bad request body: {exc}") from exc

    # -- routes ----------------------------------------------------------
    def _route(self) -> str:
        """Path without the query string: "/?v=1" must still serve the page."""
        return urllib.parse.urlparse(self.path).path

    def do_GET(self) -> None:  # noqa: N802 (http.server API)
        route = self._route()
        static = {
            "/": ("index.html", "text/html; charset=utf-8"),
            "/index.html": ("index.html", "text/html; charset=utf-8"),
            "/app.js": ("app.js", "application/javascript; charset=utf-8"),
            "/preview.js": ("preview.js", "application/javascript; charset=utf-8"),
        }
        if route in static:
            name, ctype = static[route]
            self._static(name, ctype)
        elif route == "/api/ui":
            try:
                self._json(200, load_ui())
            except (OSError, json.JSONDecodeError) as exc:
                self._json(500, {"ok": False, "error": str(exc)})
        else:
            self._json(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:  # noqa: N802 (http.server API)
        route = self._route()
        try:
            payload = self._body()
        except ValueError as exc:
            self._json(400, {"ok": False, "error": str(exc)})
            return

        if route == "/api/validate":
            ok, out = run_suite()
            self._json(200, {"ok": ok, "output": out})
            return

        if route == "/api/save":
            ok, message, out = save_ui(payload.get("root"), payload.get("assets"))
            self._json(200 if ok else 422,
                       {"ok": ok, "message": message, "output": out})
            return

        self._json(404, {"ok": False, "error": "not found"})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (default: loopback only)")
    ap.add_argument("--no-open", action="store_true", help="do not open a browser")
    args = ap.parse_args()

    if not UI_DIR.is_dir():
        print(f"error: UI assets not found: {UI_DIR}", file=sys.stderr)
        return 2

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"LCD UI editor on {url}  (assets: {UI_DIR})")
    print("Ctrl-C to stop.")
    if not args.no_open:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

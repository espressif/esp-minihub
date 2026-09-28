#!/usr/bin/env python3
"""
MiniHub 本地开发同步面板：在浏览器里上传/下载 Web、adapters.json、类型目录 CSV。

设备 API 没有 CORS，因此本脚本在本机起一个小服务：提供 UI，并把 /hub/* 代理到设备。

用法:
  python3 tools/dev_sync.py              # http://127.0.0.1:8765/
  python3 tools/dev_sync.py --port 9000
  python3 tools/dev_sync.py --open       # 启动后尝试打开浏览器

环境变量（可选）:
  MINIHUB_DEV_BASE   默认设备地址，如 http://192.168.1.50
  MINIHUB_DEV_TOKEN  Bearer token（与设备 CONFIG / NVS 一致）
"""
from __future__ import annotations

import argparse
import os
import sys
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

HERE = Path(__file__).resolve().parent
HTML_PATH = HERE / "dev_sync.html"
DEFAULT_PORT = 8765
PROXY_PREFIX = "/hub/"
MAX_PROXY_BYTES = 2 * 1024 * 1024


def _normalize_device_base(raw: str) -> tuple[str | None, str | None]:
    """Return (base_url, error_message). Accepts bare host/IP by prepending http://."""
    base = (raw or "").strip().rstrip("/")
    if not base:
        return None, "missing X-MiniHub-Base (device URL); set it in the page or MINIHUB_DEV_BASE"
    if "://" not in base:
        base = "http://" + base
    if not (base.startswith("http://") or base.startswith("https://")):
        return None, "X-MiniHub-Base must be http(s) URL"
    return base, None


def _default_device_base() -> str:
    env = (os.environ.get("MINIHUB_DEV_BASE") or "").strip().rstrip("/")
    if env:
        base, err = _normalize_device_base(env)
        if base:
            return base
    return "http://minihub.local"


def _read_html() -> bytes:
    if not HTML_PATH.is_file():
        raise SystemExit(f"missing UI file: {HTML_PATH}")
    html = HTML_PATH.read_text(encoding="utf-8")
    # Prefill device URL from env when set (keeps page default otherwise).
    default = _default_device_base().replace('"', "&quot;")
    html = html.replace('value="http://minihub.local"', f'value="{default}"', 1)
    return html.encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "MiniHubDevSync/1.0"

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))

    def log_request(self, code: "int | str" = "-", size: "int | str" = "-") -> None:
        reason = getattr(self, "_log_reason", None)
        self._log_reason = None
        if reason:
            self.log_message('"%s" %s %s — %s', self.requestline, str(code), str(size), reason)
        else:
            self.log_message('"%s" %s %s', self.requestline, str(code), str(size))

    def _send(self, code: int, body: bytes, content_type: str, extra: dict | None = None) -> None:
        if (
            code >= 400
            and body
            and len(body) < 240
            and content_type.startswith("text/")
        ):
            try:
                self._log_reason = body.decode("utf-8", errors="replace").strip()
            except Exception:  # noqa: BLE001
                self._log_reason = None
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def do_HEAD(self) -> None:
        self.do_GET(head_only=True)

    def do_GET(self, head_only: bool = False) -> None:
        path = urlparse(self.path).path
        if path in ("/", "/index.html", "/dev_sync.html"):
            body = b"" if head_only else _read_html()
            self._send(200, body, "text/html; charset=utf-8")
            return
        if path.startswith(PROXY_PREFIX):
            self._proxy("GET")
            return
        self._send(404, b"not found", "text/plain; charset=utf-8")

    def do_PUT(self) -> None:
        if urlparse(self.path).path.startswith(PROXY_PREFIX):
            self._proxy("PUT")
            return
        self._send(404, b"not found", "text/plain; charset=utf-8")

    def do_POST(self) -> None:
        if urlparse(self.path).path.startswith(PROXY_PREFIX):
            self._proxy("POST")
            return
        self._send(404, b"not found", "text/plain; charset=utf-8")

    def do_OPTIONS(self) -> None:
        # Same-origin UI; keep OPTIONS harmless for tooling.
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers",
                         "Authorization, Content-Type, X-MiniHub-Base")
        self.end_headers()

    def _proxy(self, method: str) -> None:
        parsed = urlparse(self.path)
        rel = unquote(parsed.path[len(PROXY_PREFIX):])
        if not rel or rel.startswith("/") or ".." in rel.split("/"):
            self._send(400, b"bad proxy path", "text/plain; charset=utf-8")
            return

        raw_base = self.headers.get("X-MiniHub-Base") or os.environ.get("MINIHUB_DEV_BASE", "")
        base, base_err = _normalize_device_base(raw_base)
        if base_err or not base:
            self._send(400, (base_err or "bad device base").encode(), "text/plain; charset=utf-8")
            return

        target = f"{base}/{rel}"
        if parsed.query:
            target = f"{target}?{parsed.query}"

        length = int(self.headers.get("Content-Length") or "0")
        if length > MAX_PROXY_BYTES:
            self._send(413, b"body too large", "text/plain; charset=utf-8")
            return
        body = self.rfile.read(length) if length > 0 else None

        req = urllib.request.Request(target, data=body, method=method)
        ctype = self.headers.get("Content-Type")
        if ctype:
            req.add_header("Content-Type", ctype)
        auth = self.headers.get("Authorization")
        if not auth:
            token = os.environ.get("MINIHUB_DEV_TOKEN", "").strip()
            if token:
                auth = f"Bearer {token}"
        if auth:
            req.add_header("Authorization", auth)
        # Avoid compressing binary/csv oddly on some stacks
        req.add_header("Accept", "*/*")

        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read(MAX_PROXY_BYTES + 1)
                if len(data) > MAX_PROXY_BYTES:
                    self._send(502, b"upstream body too large", "text/plain; charset=utf-8")
                    return
                out_type = resp.headers.get("Content-Type") or "application/octet-stream"
                self._send(resp.getcode() or 200, data, out_type)
        except urllib.error.HTTPError as e:
            data = e.read(MAX_PROXY_BYTES)
            out_type = e.headers.get("Content-Type") if e.headers else "text/plain"
            self._send(e.code, data or str(e).encode(), out_type or "text/plain; charset=utf-8")
        except Exception as e:  # noqa: BLE001 — surface to UI
            msg = f"proxy error: {e}".encode()
            self._send(502, msg, "text/plain; charset=utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--open", action="store_true", help="open browser after start")
    args = ap.parse_args()

    _read_html()  # fail fast
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"MiniHub dev sync: {url}")
    print("  Set device base URL in the page (e.g. http://192.168.x.x or http://minihub.local)")
    print("  Requests go through /hub/* → device (avoids browser CORS).")
    print("  400 from this server usually means missing device URL (X-MiniHub-Base / MINIHUB_DEV_BASE).")
    env_base = (os.environ.get("MINIHUB_DEV_BASE") or "").strip()
    if env_base:
        print(f"  MINIHUB_DEV_BASE={env_base}")
    if args.open:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

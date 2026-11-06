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
import json
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

    def do_DELETE(self) -> None:
        if urlparse(self.path).path.startswith(PROXY_PREFIX):
            self._proxy("DELETE")
            return
        self._send(404, b"not found", "text/plain; charset=utf-8")

    def do_OPTIONS(self) -> None:
        # Same-origin UI; keep OPTIONS harmless for tooling.
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS")
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


class HubClient:
    """Thin REST client for the MiniHub device store/module management API."""

    def __init__(self, base: str, token: str) -> None:
        norm, err = _normalize_device_base(base or "")
        if err or not norm:
            raise SystemExit("device base URL: " + (err or
                             "missing (set --base or MINIHUB_DEV_BASE)"))
        self.base = norm
        self.token = (token or "").strip()

    def _request(self, method: str, rel: str, body=None):
        url = f"{self.base}/{rel.lstrip('/')}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if self.token:
            req.add_header("Authorization", f"Bearer {self.token}")
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                raw = resp.read().decode("utf-8", "replace")
                return resp.getcode(), (_json_loads(raw) if raw else None)
        except urllib.error.HTTPError as e:
            raw = e.read().decode("utf-8", "replace")
            return e.code, (raw or str(e))
        except Exception as e:  # noqa: BLE001
            raise SystemExit(f"request to {url} failed: {e}")

    def get(self, rel):
        return self._request("GET", rel)

    def post(self, rel, body=None):
        return self._request("POST", rel, body)


def _json_loads(s):
    try:
        return json.loads(s)
    except Exception:  # noqa: BLE001
        return s


def _print_err(code, resp) -> None:
    print(f"device returned HTTP {code}")
    if isinstance(resp, str) and resp:
        print(resp[:200])
    elif isinstance(resp, dict):
        print(json.dumps(resp, ensure_ascii=False)[:200])


def cmd_list(client: HubClient) -> int:
    code, modules = client.get("api/modules")
    if code >= 400 or not isinstance(modules, (list, dict)):
        _print_err(code, modules)
        return 1
    if isinstance(modules, dict):
        modules = modules.get("modules", [])
    _, catalog = client.get("api/store/catalog")
    cat = {}
    if isinstance(catalog, list):
        cat = {x["id"]: x for x in catalog
               if isinstance(x, dict) and x.get("id")}

    rows = []
    for m in modules:
        mid = m.get("id", "?")
        c = cat.get(mid, {})
        installed = "yes" if c.get("installed") else (
            "builtin" if m.get("managed") is False else "no")
        rows.append((
            mid,
            m.get("name", "") or c.get("name", ""),
            m.get("version", "") or c.get("version", ""),
            m.get("adapter", "") or c.get("adapter", ""),
            "yes" if m.get("enabled") else "no",
            installed,
            "yes" if c.get("upgrade_available") else "",
        ))
    if not rows:
        print("no modules reported by device")
        return 0
    hdr = ("ID", "NAME", "VERSION", "ADAPTER", "ENABLED", "INSTALLED", "UPGRADE")
    widths = [max(len(str(r[i])) for r in rows + [hdr]) for i in range(len(hdr))]
    line = "  ".join(h.ljust(widths[i]) for i, h in enumerate(hdr))
    print(line)
    print("-" * len(line))
    for r in rows:
        print("  ".join(str(x).ljust(widths[i]) for i, x in enumerate(r)))
    return 0


def _store_action(client: HubClient, action: str, mid: str, mtype: str) -> int:
    code, resp = client.post(f"api/store/{action}",
                             {"type": mtype, "id": mid, "reload": True})
    ok = 200 <= code < 300
    print(f"{action} {mtype} {mid}: {'OK' if ok else 'FAILED (HTTP ' + str(code) + ')'}")
    if isinstance(resp, dict):
        for k in ("result", "message", "error"):
            if resp.get(k):
                print(f"  {k}: {resp[k]}")
    elif isinstance(resp, str) and resp:
        print("  " + resp[:200])
    return 0 if ok else 1


def _run_cli(args) -> int:
    client = HubClient(args.base, args.token)
    if args.cmd == "list":
        return cmd_list(client)
    if args.cmd == "install":
        return _store_action(client, "install", args.id, args.type)
    if args.cmd == "uninstall":
        return _store_action(client, "uninstall", args.id, args.type)
    if args.cmd == "update":
        return _store_action(client, "upgrade", args.id, args.type)
    return 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=os.environ.get("MINIHUB_DEV_BASE", ""),
                    help="device base URL for CLI subcommands (or MINIHUB_DEV_BASE)")
    ap.add_argument("--token", default=os.environ.get("MINIHUB_DEV_TOKEN", ""),
                    help="Bearer token for CLI subcommands (or MINIHUB_DEV_TOKEN)")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("list", help="list installed modules/plugins on the device")
    pi = sub.add_parser("install", help="install a module from the store by id")
    pi.add_argument("id", help="module or pack id")
    pi.add_argument("--type", default="module", choices=["module", "pack"])
    pu = sub.add_parser("uninstall", help="uninstall a module by id")
    pu.add_argument("id")
    pu.add_argument("--type", default="module", choices=["module", "pack"])
    pud = sub.add_parser("update", help="update (upgrade) an installed module by id")
    pud.add_argument("id")
    pud.add_argument("--type", default="module", choices=["module", "pack"])
    ap.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--open", action="store_true", help="open browser after start")
    args = ap.parse_args()

    if args.cmd:
        return _run_cli(args)

    _read_html()  # fail fast
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"MiniHub dev sync: {url}")
    print("  CLI: list | install <id> | uninstall <id> | update <id>")
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

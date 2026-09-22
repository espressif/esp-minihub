#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Minimal MiniHub Store server for LAN testing (default :8080).

Serves catalog.json and /files/<id>.mhub from the deploy directory:

  MINIHUB_PLUGINS_DEPLOY_DIR  (default ~/workspace/minihub_store)

Layout: catalog.json and <id>/<id>.mhub (zip only; MHMD .so stays in repo build/).
Sources remain under examples/plugins/ or MINIHUB_PLUGIN_DIRS.

Vendor / cloud protocol helpers belong in the Store plugin repo, not this Host tool.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import module_source_dir, resolve_deploy_dir  # noqa: E402


def load_catalog(catalog: Path) -> list:
    if catalog.exists():
        return json.loads(catalog.read_text(encoding="utf-8"))
    return []


def resolve_store_file(deploy: Path, name: str) -> Path | None:
    """Map /files/<id>.mhub → deploy/<id>/<id>.mhub; .json falls back to source module.json."""
    if not name or "/" in name or name.startswith("."):
        return None
    path = Path(name)
    stem, suffix = path.stem, path.suffix.lower()
    mod_dir = deploy / stem
    if suffix == ".json":
        src = module_source_dir(stem) / "module.json"
        return src if src.is_file() else None
    if not mod_dir.is_dir():
        return None
    if suffix == ".mhub":
        candidate = mod_dir / f"{stem}.mhub"
        return candidate if candidate.is_file() else None
    return None


def make_handler(deploy: Path, catalog: Path):
    class Handler(BaseHTTPRequestHandler):
        def _cors(self) -> None:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")

        def _json(self, code: int, obj) -> None:
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self._cors()
            self.end_headers()
            self.wfile.write(body)

        def do_OPTIONS(self) -> None:
            self.send_response(204)
            self._cors()
            self.end_headers()

        def do_GET(self) -> None:
            if self.path == "/api/catalog":
                self._json(200, load_catalog(catalog))
                return
            if self.path.startswith("/files/"):
                name = self.path.split("/files/", 1)[1]
                path = resolve_store_file(deploy, name)
                if not path:
                    self.send_error(404)
                    return
                data = path.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self._cors()
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_error(404)

        def log_message(self, fmt: str, *args) -> None:
            print(f"[store] {self.address_string()} {fmt % args}")

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MINIHUB_STORE_BIND", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("MINIHUB_STORE_PORT", "8080")))
    parser.add_argument(
        "--deploy",
        type=Path,
        default=None,
        help="deploy root (default: MINIHUB_PLUGINS_DEPLOY_DIR or ~/workspace/minihub_store)",
    )
    args = parser.parse_args()
    deploy = resolve_deploy_dir(args.deploy)
    catalog = deploy / "catalog.json"
    Handler = make_handler(deploy, catalog)
    srv = HTTPServer((args.host, args.port), Handler)
    print(f"MiniHub Store listening on http://{args.host}:{args.port}")
    print(f"  deploy:  {deploy}")
    print(
        f"  catalog: {catalog if catalog.exists() else '(missing — run sync_store_catalog.py)'}"
    )
    print(f"  modules: {deploy}/<id>/<id>.mhub")
    srv.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

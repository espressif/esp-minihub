# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Route map for the Host REST API, read out of ``minihub_web.c`` at build time.

Every ``httpd_uri_t`` that ``minihub_web_attach()`` actually registers becomes a
row: its URI, its method, whether its handler calls ``api_auth(req)``, and the
Kconfig gate it sits under (if any). Nothing is typed by hand, so a route that is
added, gated or un-gated shows up in the guide on the next build.

Grouping is the only judgement left in this file — see GROUPS. Keep a group name to
what the reader is doing, not to the C struct that holds the route.
"""

from __future__ import annotations

import re
from pathlib import Path

from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.statemachine import StringList
from sphinx.errors import ExtensionError

from minihub_catalog_tables import _table

ROOT = Path(__file__).resolve().parents[2]
WEB_SRC = ROOT / "components" / "minihub_web" / "minihub_web.c"

# uri prefix (after /api/) -> group key. First match wins, longest prefix first.
GROUPS = [
    ("auth", ("auth",)),
    ("devices", ("points", "devices", "command", "adapters", "device_widgets",
                 "events", "location")),
    ("config", ("config",)),
    ("store", ("store",)),
    ("update", ("update", "ota")),
    ("automation", ("rules", "scenes")),
    ("modules", ("modules", "packs", "hub")),
    ("radios", ("zigbee", "ble_mesh", "matter", "thread", "radio")),
    ("net", ("net", "system")),
    ("dev", ("dev/",)),
    ("pages", ("",)),
]

TITLES = {
    "pages": ("Pages and static assets", "页面与静态资源"),
    "auth": ("Login and session", "登录与会话"),
    "devices": ("Points and devices", "点位与设备"),
    "config": ("Configuration", "配置"),
    "store": ("Store", "商店"),
    "update": ("Update and OTA", "更新与 OTA"),
    "automation": ("Rules and scenes", "规则与场景"),
    "modules": ("Modules, packs and Hub", "模块、包与 Hub"),
    "radios": ("Radio-specific", "按射频划分"),
    "net": ("Network and system", "网络与系统"),
    "dev": ("Developer endpoints", "开发用接口"),
}


def _attach_body(text: str) -> str:
    start = text.index("minihub_web_attach")
    return text[start:]


def _registrations(body: str) -> list[dict]:
    """One record per registered route, in source order, with its #if gate."""
    out: list[dict] = []
    gated: list[str] = []
    for line in body.splitlines():
        s = line.strip()
        m = re.match(r"#if(?:def)?\s+(\S+)", s)
        if m:
            gated.append(m.group(1))
            continue
        if re.match(r"#(else|elif)\b", s):
            gated[-1:] = ["!"]
            continue
        if s.startswith("#endif"):
            if gated:
                gated.pop()
            continue
        m = re.search(r"minihub_web_add_uri\(server,\s*&(\w+)\)", s)
        if m:
            rec = _find_struct(body, m.group(1))
            if rec:
                rec["gate"] = gated[-1] if gated else ""
                out.append(rec)
    return out


def _find_struct(body: str, var: str) -> dict | None:
    m = re.search(rf"httpd_uri_t\s+{re.escape(var)}\s*=\s*\{{(.*?)\}};", body, re.S)
    if not m:
        return None
    inner = " ".join(m.group(1).split())
    uri = re.search(r"\.uri\s*=\s*([^,]+)", inner)
    method = re.search(r"\.method\s*=\s*HTTP_(\w+)", inner)
    handler = re.search(r"\.handler\s*=\s*(\w+)", inner)
    if not (uri and method and handler):
        return None
    raw_uri = uri.group(1).strip().strip('"')
    if not raw_uri.startswith("/"):
        raw_uri = _expand_macro(raw_uri)
    return {"uri": raw_uri, "method": method.group(1), "handler": handler.group(1),
            "var": var}


_MACRO_FILES = [
    WEB_SRC,
    ROOT / "components" / "minihub_config" / "include" / "minihub_paths.h",
    ROOT / "components" / "minihub_device_widget" / "include" / "minihub_device_widget.h",
]
_TEXT_CACHE: dict[Path, str] = {}


def _file_text(path: Path) -> str:
    if path not in _TEXT_CACHE:
        if not path.is_file():
            raise ExtensionError(f"minihub-routes: {path} not found")
        _TEXT_CACHE[path] = path.read_text(encoding="utf-8", errors="replace")
    return _TEXT_CACHE[path]


def _string_of(token: str) -> str | None:
    """A macro or a local ``const char *`` that holds a URI fragment."""
    patterns = (
        rf"^\s*#define\s+{re.escape(token)}\s+\"([^\"]*)\"",
        rf"{re.escape(token)}\s*=\s*\"([^\"]*)\"",
    )
    for path in _MACRO_FILES:
        text = _file_text(path)
        for pat in patterns:
            m = re.search(pat, text, re.M)
            if m:
                return m.group(1)
    return None


def _expand_macro(raw: str) -> str:
    """Resolve a URI written as ``MINIHUB_IMAGES_URI_PREFIX "*"``."""
    out = ""
    for token in re.findall(r'"[^"]*"|[A-Za-z_]\w*', raw):
        if token.startswith('"'):
            out += token.strip('"')
            continue
        val = _string_of(token)
        if val is None:
            raise ExtensionError(f"minihub-routes: cannot resolve {token} in URI {raw!r}")
        out += val
    return out


def _handler_calls_auth(text: str, handler: str) -> bool | None:
    """True/False if the handler body was found, None when it could not be located."""
    m = re.search(rf"static esp_err_t {re.escape(handler)}\(httpd_req_t \*req\)\n\{{", text)
    if not m:
        return None
    rest = text[m.end():]
    depth = 1
    for i, ch in enumerate(rest):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                body = rest[:i]
                return "api_auth(" in body or "api_auth_any(" in body
    return None


def routes() -> list[dict]:
    text = WEB_SRC.read_text(encoding="utf-8", errors="replace")
    recs = _registrations(_attach_body(text))
    missing = []
    for r in recs:
        auth = _handler_calls_auth(text, r["handler"])
        if auth is None:
            missing.append(r["handler"])
            auth = False
        r["auth"] = auth
        r["group"] = _group_of(r["uri"])
    if missing:
        raise ExtensionError("minihub-routes: handler body not found for "
                             + ", ".join(sorted(set(missing))))
    return recs


def _group_of(uri: str) -> str:
    if not uri.startswith("/api/"):
        return "pages"
    tail = uri[len("/api/"):]
    if not tail:
        return "pages"          # GET /api/ — the endpoint index itself
    for key, prefixes in GROUPS:
        if key == "pages":
            continue
        for p in prefixes:
            if p and tail.startswith(p):
                return key
    return "devices"


def routes_rst(group: str, lang: str) -> list[str]:
    zh = lang.startswith("zh")
    en_title, zh_title = TITLES[group]
    header = (["接口", "方法", "需要登录", "Kconfig 开关"] if zh
              else ["Endpoint", "Method", "Login", "Kconfig gate"])
    rows = []
    for r in routes():
        if r["group"] != group:
            continue
        rows.append([
            f"``{r['uri']}``",
            r["method"],
            ("是" if zh else "yes") if r["auth"] else ("否" if zh else "no"),
            f"``{r['gate']}``" if r["gate"] else "—",
        ])
    if not rows:
        raise ExtensionError(f"minihub-routes: no routes in group {group!r}")
    out = _table(header, [34, 10, 10, 46], rows)
    out[0] = f".. list-table:: {zh_title if zh else en_title}"
    return out


class RoutesTable(Directive):
    required_arguments = 1
    optional_arguments = 0
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        group = self.arguments[0].strip().lower()
        if group not in TITLES:
            raise self.error(f"unknown route group {group!r} (use one of "
                             f"{', '.join(TITLES)})")
        node = nodes.Element()
        self.state.nested_parse(StringList(routes_rst(group, env.config.language)), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-routes", RoutesTable)
    return {"parallel_read_safe": True, "version": "1.0"}

# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Error reference, read out of the HTTP server and the MCP gateway at build time.

Three tables, all derived from the sources:

``http``     every status the Host can answer with, how many places produce it, and
             the phrase a client sees.
``messages`` every literal error string handed to ``httpd_resp_send_err()``, with the
             endpoints that can return it — this is what a reader greps when they are
             staring at ``invalid json`` and nothing else. A string returned from
             inside a worker or a helper is named by that function instead, because no
             URI owns the reply.
``mcp``      the ``code`` values the on-device MCP tools return.

Body text built from ``esp_err_to_name(err)`` is a runtime value, so it is described
rather than listed. A glossary row is written by hand (the status list is short and the
wording matters); ``verify()`` fails the build when the code starts returning something
the tables do not know about, or keeps a row for something that no longer exists.
"""

from __future__ import annotations

import re
from pathlib import Path

from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.statemachine import StringList
from sphinx.errors import ExtensionError

from minihub_catalog_tables import _table
from minihub_routes import routes

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "components" / "minihub_web" / "minihub_web.c"
MCP_DIR = ROOT / "components" / "minihub_mcp_server"

_SEND = re.compile(
    r"httpd_resp_send_err\(\s*req\s*,\s*(?:HTTPD_(\d{3})_[A-Z_0-9]+|(\d{3}))"
    r"\s*,\s*(?:\"([^\"]*)\")?")
_SET = re.compile(r"httpd_resp_set_status\(\s*req\s*,\s*\"(\d{3}) ([^\"]+)\"")
_HANDLER = re.compile(r"^static\s+\w+\s+(\w+)\s*\(", re.M)
_ERR_OBJ = re.compile(r"err_obj\(")
_MCP_ERR = re.compile(r"mcp_error\(")
_CODE_ARG = re.compile(r"\"([A-Z][A-Z_0-9]{2,})\"")

STATUS_GLOSS = {
    "400": ("bad or missing body, an unknown parameter, or a path the handler rejects",
            "请求体缺失或不合法、参数不认识、或处理函数拒绝的路径"),
    "401": ("no session cookie and no accepted Bearer token",
            "既没有会话 cookie，也没有被接受的 Bearer token"),
    "403": ("authenticated, but not allowed to do this", "已登录但不允许做这件事"),
    "404": ("no route, no file, no such point or module", "没有这条路由、没有这个文件、点位或模块不存在"),
    "409": ("something already occupies the slot: an OTA is in progress, or the "
            "password has already been set", "位置已经被占了：正在跑一次 OTA，或密码早已设置过"),
    "500": ("the request reached its handler and the handler failed; the detail is "
            "the failure it hit (``oom`` is memory, otherwise an ``esp_err`` name)",
            "请求进到处理函数后失败了；正文就是失败原因（``oom`` 是内存不足，其余是 ``esp_err`` 名）"),
    "502": ("an upstream the Hub proxies for failed (deploy server, cloud)",
            "Hub 代为请求的上游失败了（部署服务器、云）"),
}

MCP_GLOSS = {
    "HUB_NOT_READY": ("a Hub reload is in flight; retry", "Hub 正在重载；稍后重试"),
    "MODULE_NOT_READY": ("the adapter or module is not up yet", "适配器或模块还没起来"),
    "CONFIG_NOT_READY": ("the config document has not been read yet", "配置文档还没读进来"),
    "RATE_LIMITED": ("called too often; the tool is throttled", "调用过频；该工具有节流"),
    "NOT_IMPLEMENTED": ("the path is not served by the gateway", "该路径不由网关处理"),
    "DENIED": ("the path or method is outside the ``hub.api`` allowlist",
               "该路径或方法不在 ``hub.api`` 白名单内"),
    "INVALID_BODY": ("the request had no JSON body", "请求没有 JSON body"),
    "INVALID_JSON": ("the body is present but does not parse", "body 有内容但解析不了"),
    "INVALID_PARAMS": ("arguments do not match the tool's schema", "参数不符合该工具的 schema"),
    "INVALID_ARG": ("one argument is unusable as given", "某个参数按给定值不可用"),
    "VALIDATION_FAILED": ("parsed, but the document shape is wrong",
                          "解析成功，但文档结构不对"),
    "NOT_FOUND": ("no such point, module or file", "点位、模块或文件不存在"),
    "SET_FAILED": ("a point write did not land", "写点位没落下去"),
    "SAVE_FAILED": ("the document could not be written to storage", "文档没能写入存储"),
    "RELOAD_FAILED": ("saved, but the Hub could not reload it", "已保存，但 Hub 没能重载它"),
    "LIST_FAILED": ("the list could not be assembled", "列表拼不出来"),
    "INVOKE_FAILED": ("the command was routed and failed", "命令已路由但失败"),
    "STORE_ERROR": ("the Store (catalog, install) failed", "商店侧（目录、安装）失败"),
    "STORE_CONFIG_FAILED": ("the Store configuration could not be read or written",
                            "商店配置读不到或写不进"),
    "MODULE_ERROR": ("a module call failed", "模块调用失败"),
    "INTERNAL": ("an unexpected internal failure; the detail names it",
                 "意料之外的内部失败，detail 会写明是哪一个"),
}


_CACHE: dict[Path, str] = {}


def _text(path: Path) -> str:
    """Sources are scanned file by file, so read each one once per build."""
    if path not in _CACHE:
        if not path.is_file():
            raise ExtensionError(f"minihub-errors: {path} not found")
        _CACHE[path] = path.read_text(encoding="utf-8", errors="replace")
    return _CACHE[path]


def _handler_uris() -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for r in routes():
        out.setdefault(r["handler"], []).append(f"{r['method'].title()} {r['uri']}")
    return {k: sorted(set(v)) for k, v in out.items()}


def _sources() -> list[Path]:
    """Every file that can answer a request, found by what it calls.

    ``minihub_web.c`` registers most URIs, but the WebSocket, camera and Matter OTA
    handlers answer errors too, so the list is derived rather than hardcoded: a new
    component that reports an error joins the table by itself.
    """
    out = [WEB]
    for path in sorted((ROOT / "components").rglob("*")):
        if path.suffix not in (".c", ".cpp") or not path.is_file():
            continue
        if path == WEB:
            continue
        if "httpd_resp_send_err" in _text(path):
            out.append(path)
    return out


def _sites() -> list[tuple[str, int, str, str | None, str]]:
    """(file, line, status, literal message or None, enclosing handler)."""
    out = []
    for path in _sources():
        text = _text(path)
        starts = [(m.start(), m.group(1)) for m in _HANDLER.finditer(text)]
        for m in _SEND.finditer(text):
            line = text[:m.start()].count("\n") + 1
            handler = next((h for pos, h in reversed(starts) if pos < m.start()),
                           path.stem)
            out.append((path.name, line, m.group(1) or m.group(2), m.group(3), handler))
        for m in _SET.finditer(text):
            if m.group(1)[0] in "45":
                line = text[:m.start()].count("\n") + 1
                handler = next((h for pos, h in reversed(starts) if pos < m.start()),
                               path.stem)
                out.append((path.name, line, m.group(1), None, handler))
    return out


def statuses() -> list[tuple[str, int, str]]:
    """(status, places, first line) for every status the server can emit."""
    agg: dict[str, list[int]] = {}
    for _f, line, status, _msg, _h in _sites():
        agg.setdefault(status, []).append(line)
    return [(s, len(v), min(v)) for s, v in sorted(agg.items())]


def messages() -> list[tuple[str, str, int, list[str]]]:
    """(message, statuses, places, endpoints) for every literal error string.

    One string can be returned under two statuses when the same read fails in two
    different ways, so the status column is a list, not a single code.
    """
    per_handler = _handler_uris()
    agg: dict[str, dict] = {}
    for _file, _line, status, msg, handler in _sites():
        if not msg:
            continue
        a = agg.setdefault(msg, {"statuses": set(), "places": 0, "ends": set()})
        a["statuses"].add(status)
        a["places"] += 1
        a["ends"].update(per_handler.get(handler, [handler]))
    return sorted((m, "/".join(sorted(a["statuses"])), a["places"], sorted(a["ends"]))
                  for m, a in agg.items())


def _call_args(text: str, open_at: int) -> str:
    """The inside of the call whose ``(`` sits at ``open_at``."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_at + 1:i]
    return text[open_at + 1:]


def mcp_codes() -> list[tuple[str, int, int]]:
    """(code, places, first line) across the MCP sources.

    A code is whatever literal appears in the call, because several sites pick it
    with a ternary; ``esp_err_to_name(err)`` in the same argument list means the
    gateway can also return a bare ``ESP_*`` name that no source spells out.
    """
    agg: dict[str, list[int]] = {}
    for path in sorted(MCP_DIR.rglob("*")):
        if path.suffix not in (".c", ".h") or not path.is_file():
            continue
        text = _text(path)
        for opener in (_ERR_OBJ, _MCP_ERR):
            for m in opener.finditer(text):
                line = text[:m.start()].count("\n") + 1
                for lit in _CODE_ARG.finditer(_call_args(text, m.end() - 1)):
                    agg.setdefault(lit.group(1), []).append(line)
    return [(c, len(v), min(v)) for c, v in sorted(agg.items())]


def verify() -> list[str]:
    """The glossary and the sources must describe the same set of codes."""
    problems = []
    live = {status for status, _n, _line in statuses()}
    for status in sorted(live - set(STATUS_GLOSS)):
        problems.append(f"HTTP {status} is returned but has no glossary row")
    for status in sorted(set(STATUS_GLOSS) - live):
        problems.append(f"glossary keeps HTTP {status}, which nothing returns any more")
    codes = {code for code, _n, _line in mcp_codes()}
    for code in sorted(codes - set(MCP_GLOSS)):
        problems.append(f"MCP code {code} is returned but has no glossary row")
    for code in sorted(set(MCP_GLOSS) - codes):
        problems.append(f"MCP glossary keeps {code}, which nothing returns any more")
    return problems


def _rows(kind: str, lang: str) -> list[str]:
    zh = lang.startswith("zh")
    problems = verify()
    if problems:
        raise ExtensionError("minihub-errors is out of date:\n  " + "\n  ".join(problems))
    header, rows = None, None
    if kind == "http":
        header = (["状态", "出现处数", "含义"] if zh
                  else ["Status", "Sites", "Meaning"])
        rows = [[f"``{s}``", str(n), (STATUS_GLOSS[s][1] if zh else STATUS_GLOSS[s][0])]
                for s, n, _line in statuses()]
        widths = [8, 10, 82]
        caption = "HTTP 状态" if zh else "HTTP statuses"
    elif kind == "messages":
        header = (["正文", "状态", "处数", "由哪些端点或函数返回"] if zh
                  else ["Body", "Status", "Sites", "Returned by"])
        rows = []
        for msg, status, count, ends in messages():
            shown = ", ".join(f"``{e}``" for e in ends[:3]) + (" …" if len(ends) > 3 else "")
            rows.append([f"``{msg}``", f"``{status}``", str(count), shown or "—"])
        widths = [25, 9, 6, 60]
        caption = "错误正文" if zh else "Error bodies"
    elif kind == "mcp":
        header = (["``code``", "出现处数", "含义"] if zh
                  else ["``code``", "Sites", "Meaning"])
        rows = [[f"``{c}``", str(n), (MCP_GLOSS[c][1] if zh else MCP_GLOSS[c][0])]
                for c, n, _line in mcp_codes()]
        widths = [22, 10, 68]
        caption = "MCP 错误码" if zh else "MCP error codes"
    else:
        raise ExtensionError(f"minihub-errors: unknown table {kind!r}")
    out = _table(header, widths, rows)
    out[0] = f".. list-table:: {caption}"
    return out


class ErrorsTable(Directive):
    required_arguments = 1
    optional_arguments = 0
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        node = nodes.Element()
        self.state.nested_parse(StringList(
            _rows(self.arguments[0].strip().lower(), env.config.language)), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-errors", ErrorsTable)
    return {"parallel_read_safe": True, "version": "1.0"}

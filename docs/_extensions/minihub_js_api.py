# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""The ``host`` object an L2 plugin receives, read out of the JS glue at build time.

``make_host_object()`` in ``components/minihub_js/minihub_js.c`` builds the object
namespace by namespace, and the per-namespace extension TUs add the rest. Each
``JS_SetPropertyStr`` line names the method, the C function behind it and its
argument count, so the guide can print the real surface instead of a list someone
keeps in sync by hand.

A method the parser cannot place is a build error: that is how a renamed or removed
``host.*`` call stops living on in the programming guide.
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

# The three builders that hand a plugin its API surface. Everything else in these
# files builds throwaway JS values (a point row, an HTTP response), which are not
# the plugin contract and must not show up here.
BUILDERS = [
    (ROOT / "components" / "minihub_js" / "minihub_js.c", "make_host_object"),
    (ROOT / "components" / "minihub_js" / "minihub_js.c", "js_timer_install_global"),
    (ROOT / "components" / "minihub_js" / "minihub_js_crypto.c", "minihub_js_attach_crypto"),
    (ROOT / "components" / "minihub_js" / "minihub_js_net.c", "minihub_js_attach_net"),
    (ROOT / "components" / "minihub_js" / "minihub_js_ble.c", "minihub_js_attach_ble"),
    (ROOT / "components" / "minihub_js" / "minihub_js_mdns.c", "minihub_js_attach_mdns"),
    (ROOT / "components" / "minihub_js" / "minihub_js_mqtt.c", "minihub_js_attach_mqtt"),
]

# The API is documented on one page per C source file, so each page asks for only
# the namespaces its builder owns. ``global`` (the timer globals) lives in
# minihub_js.c, so it belongs to ``core``.
SECTIONS = {
    "core": {"log", "point", "command", "event", "http", "config", "module",
             "timer", "adapter", "time", "global"},
    "net": {"net", "net.udp", "net.tcp", "crypto"},
    "ble": {"ble"},
    "mdns": {"mdns"},
    "mqtt": {"mqtt"},
}

# Methods are either plain JS_NewCFunction() or JS_NewCFunctionMagic(), which
# carries a per-plugin id (host.mqtt needs one to keep each plugin's filters and
# queue position apart). Only the arity is a table column, so the extra magic
# arguments are skipped over.
_CALL = re.compile(
    r"JS_SetPropertyStr\(\s*ctx\s*,\s*(\w+)\s*,\s*\"([^\"]+)\"\s*,\s*"
    r"(?:JS_NewCFunction(?:Magic)?\(\s*ctx\s*,\s*\w+\s*,\s*\"[^\"]*\"\s*,\s*(\d+)\s*(?:,[^)]*\)|\))|(\w+))",
    re.S,
)


def _body(text: str, fname: str) -> tuple[str, int]:
    """The body of one builder function and the line its ``{`` sits on.

    Only the *definition* counts: ``make_host_object(ctx)`` is also called earlier in
    the file, and taking the first hit would hand back an unrelated brace block.
    Newlines are kept so each call can cite its own line; the regex matches across
    them, which is what lets a ``JS_SetPropertyStr`` wrapped over two lines parse.
    """
    m = re.search(rf"^[A-Za-z_].*\b{re.escape(fname)}\([^)]*\)\n\{{", text, re.M)
    if not m:
        raise ExtensionError(f"minihub-js-api: no definition for {fname}")
    start = text.index("{", m.start())
    base = text[:start].count("\n") + 1
    depth = 0
    for j in range(start, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:j], base
    raise ExtensionError(f"minihub-js-api: unterminated body for {fname}")


def _calls(text: str, fname: str) -> list[tuple[str, str, str, int, bool, int]]:
    """(target var, property, referenced var, arity, is a C function, line)."""
    body, base = _body(text, fname)
    out = []
    for m in _CALL.finditer(body):
        out.append((m.group(1), m.group(2), m.group(4), int(m.group(3) or 0),
                    m.group(3) is not None, base + body[:m.start()].count("\n")))
    return out


def surface() -> list[dict]:
    """Namespace -> its methods, from the three builders only."""
    all_calls = []
    for path, fname in BUILDERS:
        if not path.is_file():
            raise ExtensionError(f"minihub-js-api: {path} not found")
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = str(path.relative_to(ROOT))
        for target, prop, value, arity, is_fn, line in _calls(text, fname):
            all_calls.append((rel, target, prop, value, arity, is_fn, line))

    # Pass 1: name the objects. An attach whose value is a variable is an object,
    # so host/net give the labels; the timer installer works on the global object.
    label: dict[str, str] = {}
    for _rel, target, prop, value, _arity, is_fn, _line in all_calls:
        if is_fn or value is None:
            continue                      # a method, not an object attach
        if target == "host":
            label[value] = prop
        elif value not in label:
            label[value] = f"{label.get(target, target)}.{prop}"
    label.setdefault("global", "global")

    # Pass 2: group the methods under those labels. Arity 0 is a real method
    # (host.config.get(), host.time.id()), so the flag decides, not the number.
    rows: dict[str, list[str]] = {}
    where: dict[str, str] = {}
    for rel, target, prop, _value, arity, is_fn, line in all_calls:
        if not is_fn or target == "host":
            continue
        ns = label.get(target, target)
        cell = f"``{prop}/{arity}``" if ns == "global" else f"``{ns}.{prop}/{arity}``"
        bucket = rows.setdefault(ns, [])
        if cell not in bucket:
            bucket.append(cell)
            where.setdefault(ns, f"{rel}:{line}")
    out = [{"ns": k, "methods": v, "where": where[k], "global": k == "global"}
           for k, v in rows.items() if v]
    if not out:
        raise ExtensionError("minihub-js-api: found no host.* namespaces")
    return out


def js_api_rst(lang: str, section: str | None = None) -> list[str]:
    zh = lang.startswith("zh")
    recs = surface()
    if section is not None:
        allowed = SECTIONS.get(section)
        if allowed is None:
            raise ExtensionError(f"minihub-js-api: unknown section {section!r}")
        recs = [r for r in recs
                if r["ns"] in allowed or (r["global"] and section == "core")]
    header = (["命名空间", "方法（斜杠后的数字是参数个数）", "定义处"] if zh
              else ["Namespace", "Methods (digits after / are the arg count)", "Defined at"])
    rows = [[("``global``" if rec["global"] else f"``host.{rec['ns']}``"),
             ", ".join(rec["methods"]),
             f"``{rec['where']}``"] for rec in recs]
    out = _table(header, [16, 58, 26], rows)
    out[0] = (".. list-table:: host 对象" if zh else ".. list-table:: The host object")
    return out


class JsApiTable(Directive):
    required_arguments = 0
    optional_arguments = 1
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        section = self.arguments[0] if self.arguments else None
        node = nodes.Element()
        self.state.nested_parse(StringList(js_api_rst(env.config.language, section)), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-js-api", JsApiTable)
    return {"parallel_read_safe": True, "version": "1.0"}

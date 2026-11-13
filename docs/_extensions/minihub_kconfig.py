# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Kconfig reference, read out of the Kconfig fragments at build time.

The authoritative list of fragments is the ``rsource`` list in
``cmake/minihub_bundle/Kconfig.projbuild`` — that is exactly what ``idf.py`` mounts, so a
component that adds a ``Kconfig.minihub`` without being rsourced, or a ``rsource`` line
whose file was deleted, fails the docs build instead of silently shrinking the table.

One table per ``menu`` title, in ``rsource`` order. Each row is the symbol, its type and
default, what has to be on before it is even visible, what the sources say it is for
(the ``help`` text verbatim, because that is what ``menuconfig`` shows the reader), and
which files read the ``CONFIG_`` macro. The last column is why this page is trustworthy:
a symbol nothing reads is visible on the page, and a symbol whose only reader is a shell
script is credited to that script.

Defaults are the Kconfig defaults. A product's ``sdkconfig`` can move any of them, and
``sdkconfig`` wins over the defaults files, so the numbers here are what a fresh build
gets, not what a specific board is currently running.
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
PROJBUILD = ROOT / "cmake" / "minihub_bundle" / "Kconfig.projbuild"

# Where a CONFIG_ macro can be read. Kept explicit so the scan cannot wander into a
# build tree, and so a new kind of consumer has to be added on purpose.
_REF_DIRS = ("components", "application", "tools", "cmake", "examples")
_REF_SUFFIX = {".c", ".h", ".cpp", ".py", ".sh", ".js", ".cmake", ".yml", ".projbuild",
               ".defaults", ".md", ".rst"}
_REF_NAMES = {"CMakeLists.txt"}
# Anything with these in its path is generated, vendored, or the guide itself.
_SKIP = ("esp-idf/", "/build/", "managed_components", "/_build/", ".venv",
         "sdkconfig", "/xml/", "docs/_static")

_CONFIG_RE = re.compile(r"^(\s*)config\s+(\w+)\s*$", re.M)
_TYPE_RE = re.compile(r"^\s*(bool|string|int|hex|tristate)(?:\s+\"([^\"]*)\")?")
_ATTR_RE = re.compile(
    r"^\s*(depends on|select|imply|default|range|prompt|help)\b(.*)$")


def _fragment_paths() -> list[Path]:
    if "fragments" not in _CACHE:
        text = _read(PROJBUILD)
        out = []
        for rel in re.findall(r'rsource\s+"([^"]+)"', text):
            path = (PROJBUILD.parent / rel).resolve()
            if not path.is_file():
                raise ExtensionError(f"minihub-kconfig: rsource target missing: {path}")
            out.append(path)
        mounted = set(out)
        on_disk = {p.resolve() for p in (ROOT / "components").rglob("Kconfig.minihub")}
        strays = sorted(str(p.relative_to(ROOT)) for p in on_disk - mounted)
        if strays:
            raise ExtensionError(
                "minihub-kconfig: these fragments exist but are not rsourced from "
                f"Kconfig.projbuild, so the build never sees them: {strays}")
        _CACHE["fragments"] = out
    return _CACHE["fragments"]  # type: ignore[return-value]


def _read(path: Path) -> str:
    if not path.is_file():
        raise ExtensionError(f"minihub-kconfig: {path} not found")
    return path.read_text(encoding="utf-8", errors="replace")


def _parse(text: str, menu: str, source: str) -> list[dict]:
    """Every ``config`` block in one fragment, with the conditions that gate it."""
    blocks = []
    matches = list(_CONFIG_RE.finditer(text))
    # ``if SYMBOL`` ... ``endif`` blocks that enclose configs, by indent.
    guards = [(m.start(), m.group(1))
              for m in re.finditer(r"^\s*if\s+([A-Za-z_]\w*)\s*$", text, re.M)]
    ends = [m.start() for m in re.finditer(r"^\s*endif\s*$", text, re.M)]
    for i, m in enumerate(matches):
        start = m.end()
        stop = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:stop]
        indent = len(m.group(1))
        guard = next((g for pos, g in guards
                      if pos < m.start() and all(e > m.start() for e in ends)), None)
        entry = {"name": m.group(2), "menu": menu, "source": source,
                 "type": "", "prompt": "", "defaults": [], "ranges": [],
                 "depends": [], "help": [], "guard": guard}
        in_help = False
        for line in body.splitlines():
            if in_help:
                if line.strip() and len(line) - len(line.lstrip()) >= indent + 8:
                    entry["help"].append(line.strip())
                    continue
                in_help = False
            stripped = line.strip()
            if not stripped:
                continue
            tm = _TYPE_RE.match(line)
            if tm and not entry["type"]:
                entry["type"] = tm.group(1)
                entry["prompt"] = tm.group(2) or ""
                continue
            am = _ATTR_RE.match(line)
            if not am:
                continue
            key, rest = am.group(1), am.group(2).strip()
            if key == "help":
                in_help = True
            elif key == "default":
                entry["defaults"].append(rest)
            elif key == "range":
                entry["ranges"].append(rest)
            elif key == "depends on":
                entry["depends"].append(rest)
            elif key == "prompt" and not entry["prompt"]:
                entry["prompt"] = rest.strip('"')
        blocks.append(entry)
    return blocks


_CACHE: dict[str, object] = {}


def _candidate_files() -> list[Path]:
    if "files" not in _CACHE:
        out = []
        for name in _REF_DIRS:
            base = ROOT / name
            if not base.is_dir():
                continue
            for path in base.rglob("*"):
                if not path.is_file():
                    continue
                rel = str(path.relative_to(ROOT))
                if path.suffix not in _REF_SUFFIX and path.name not in _REF_NAMES:
                    continue
                if any(s in rel for s in _SKIP):
                    continue
                out.append(path)
        _CACHE["files"] = out
    return _CACHE["files"]  # type: ignore[return-value]


def _ref_index(names: list[str]) -> dict[str, set[str]]:
    """Which source, script or build file mentions ``CONFIG_<name>``.

    Cached for the whole build: one walk of the repo answers all 20 tables, and a
    rescan per table would dominate the docs build.
    """
    key = ",".join(sorted(names))
    if key in _CACHE:
        return _CACHE[key]  # type: ignore[return-value]
    pattern = re.compile(r"CONFIG_(" + "|".join(sorted(names, key=len, reverse=True)) + r")\b")
    refs: dict[str, set[str]] = {n: set() for n in names}
    for path in _candidate_files():
        rel = str(path.relative_to(ROOT))
        for m in pattern.finditer(_read(path)):
            refs[m.group(1)].add(rel)
    _CACHE[key] = refs
    return refs


def symbols() -> list[dict]:
    if "symbols" not in _CACHE:
        out = []
        for path in _fragment_paths():
            text = _read(path)
            title = re.search(r'^\s*menu\s+"([^"]+)"', text, re.M)
            out.extend(_parse(text, title.group(1) if title else path.stem,
                              str(path.relative_to(ROOT))))
        _CACHE["symbols"] = out
    return _CACHE["symbols"]  # type: ignore[return-value]


def _short(rel: str) -> str:
    parts = Path(rel).parts
    if parts[0] == "components" and len(parts) > 2:
        return "/".join(parts[1:3]) if parts[1] == "minihub_protocol_adapters" \
            else f"components/{parts[1]}"
    if parts[0] == "application":
        return "/".join(parts[:2])
    return rel


def _default_of(entry: dict) -> str:
    if not entry["defaults"]:
        return "—"
    parts = []
    for d in entry["defaults"]:
        parts.append(d.replace('"', ""))
    return " / ".join(parts) if len(parts) > 1 else parts[0]


def groups() -> dict[str, list[dict]]:
    """Menu title -> rows, in ``rsource`` order."""
    out: dict[str, list[dict]] = {}
    for entry in symbols():
        out.setdefault(entry["menu"], []).append(entry)
    return out


def verify() -> list[str]:
    problems = []
    rows = symbols()
    names = [r["name"] for r in rows]
    if len(names) != len(set(names)):
        dupes = sorted({n for n in names if names.count(n) > 1})
        problems.append(f"symbol declared twice: {dupes}")
    for path in _fragment_paths():
        text = _read(path)
        if "menu " not in text:
            problems.append(f"{path.name}: no menu title, the table cannot group it")
    for r in rows:
        if not r["type"]:
            problems.append(f"{r['name']}: no bool/int/string type")
        if r["type"] in ("int", "hex") and not r["defaults"]:
            problems.append(f"{r['name']}: an int with no default is a Kconfig bug")
        if r["type"] == "string" and not r["prompt"]:
            problems.append(f"{r['name']}: string without a prompt menuconfig can show")
    return problems


def _rows(menu: str, lang: str, refs: dict[str, set[str]]) -> list[str]:
    problems = verify()
    if problems:
        raise ExtensionError("minihub-kconfig is out of date:\n  " + "\n  ".join(problems))
    zh = lang.startswith("zh")
    rows = [r for r in symbols() if r["menu"] == menu]
    if not rows:
        raise ExtensionError(
            f"minihub-kconfig: no such menu {menu!r}; the fragments declare "
            + ", ".join(sorted({r['menu'] for r in symbols()})))
    header = (["符号", "类型 / 缺省", "生效条件",
               "作用（Kconfig help 原文）与读取处"] if zh
              else ["Symbol", "Type / default", "Gated by",
                    "Purpose (Kconfig help, else the prompt) and who reads it"])
    widths = [22, 15, 15, 48]
    table_rows = []
    for r in rows:
        gates = list(r["depends"])
        if r["guard"]:
            gates.insert(0, f"``{r['guard']}``")
        kind = r["type"] or "?"
        default = _default_of(r)
        # An empty string default is a real value here (it means "no compiled-in
        # secret"), but ```` is not valid RST, so it gets a word instead.
        if default == "":
            default = "(empty)" if not zh else "（空）"
        second = f"``{default}``" if r["type"] != "bool" else default
        col2 = [f"{kind}： {second}" if zh else f"{kind}: {second}"]
        if r["ranges"]:
            col2.append(("范围 " if zh else "range ") + " / ".join(r["ranges"]))
        what = " ".join(r["help"]) or r["prompt"] or "—"
        read_by = sorted({_short(p) for p in refs.get(r["name"], [])})
        tail = ("没有文件读它" if zh else "no file reads it") if not read_by else \
            ("读取：" if zh else "read by: ") + ", ".join(
                f"``{p}``" for p in read_by[:3]) + (" …" if len(read_by) > 3 else "")
        col4 = [what, tail]
        table_rows.append([f"``{r['name']}``", col2,
                           " / ".join(gates) if gates else
                           ("始终可见" if zh else "always visible"),
                           col4])
    out = _table(header, widths, table_rows)
    out[0] = f".. list-table:: {menu}"
    return out


class KconfigTable(Directive):
    # Menu titles are two words ("JS plugins", "Web UI"), so the name is the whole
    # argument line rather than a single token.
    required_arguments = 1
    optional_arguments = 0
    final_argument_whitespace = True
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        menu = self.arguments[0].strip()
        names = [r["name"] for r in symbols()]
        refs = _ref_index(names)
        node = nodes.Element()
        self.state.nested_parse(StringList(
            _rows(menu, env.config.language, refs)), 0, node)
        return node.children


def _compose():
    """Load ``tools/compose_sdkconfig.py`` so the path rules live in one place."""
    if "compose" not in _CACHE:
        import importlib.util

        path = ROOT / "tools" / "compose_sdkconfig.py"
        if not path.is_file():
            raise ExtensionError(f"minihub-kconfig: {path} not found")
        spec = importlib.util.spec_from_file_location("minihub_compose", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _CACHE["compose"] = module
    return _CACHE["compose"]  # type: ignore[return-value]


def features(app: str) -> list[dict]:
    """The product's recipe, in the order the build feeds it to the config system."""
    compose = _compose()
    try:
        names, board = compose.app_feature_list(ROOT, app)
    except FileNotFoundError as exc:
        raise ExtensionError(f"minihub-kconfig: {exc}") from exc
    rows = []
    for name in names:
        for rel in compose.feature_layers(ROOT, app, name):
            path = ROOT / rel
            if not path.is_file():
                # stacks.gen is written by tools/build_firmware.py on every build.
                rows.append({"name": name, "path": rel, "sets": [], "generated": True})
                continue
            sets = re.findall(r"^(CONFIG_MINIHUB_\w+)=(.*)$", _read(path), re.M)
            rows.append({"name": name, "path": rel,
                         "sets": [f"{k.split('CONFIG_MINIHUB_')[1]}={v}" for k, v in sets],
                         "generated": False})
    rows.append({"name": "board", "path": f"application/{app}/configs/sdkconfig/app.yml",
                 "sets": [f"board={board}"], "generated": False})
    return rows


def _feature_rows(app: str, lang: str) -> list[str]:
    zh = lang.startswith("zh")
    header = (["顺序", "特性", "文件", "它为 MiniHub 设置了什么"] if zh
              else ["#", "Feature", "File", "What it sets for MiniHub"])
    widths = [5, 18, 40, 37]
    rows = []
    for i, f in enumerate(features(app), 1):
        note = ("构建时由 ``tools/build_firmware.py`` 写出" if zh
                else "written by tools/build_firmware.py at build time")
        rows.append([str(i), f"``{f['name']}``",
                     [f"``{f['path']}``"] + ([note] if f["generated"] else []),
                     ", ".join(f"``{s}``" for s in f["sets"][:6])
                     + (" …" if len(f["sets"]) > 6 else "")
                     or ("—" if not f["generated"] else "")])
    out = _table(header, widths, rows)
    out[0] = f".. list-table:: {app}"
    return out


class KconfigFeatures(Directive):
    required_arguments = 1
    optional_arguments = 0
    final_argument_whitespace = True
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        node = nodes.Element()
        self.state.nested_parse(StringList(
            _feature_rows(self.arguments[0].strip(), env.config.language)), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-kconfig", KconfigTable)
    app.add_directive("minihub-features", KconfigFeatures)
    return {"parallel_read_safe": True, "version": "1.0"}

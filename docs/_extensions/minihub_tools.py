# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""The PC-side script reference, read out of ``tools/`` at build time.

Every top-level script in ``tools/`` gets a row: its own first documented line as the
purpose, the flags it actually accepts, and where this guide talks about it (if anywhere).
Nothing is listed by hand, so the table cannot fall behind the directory.

``verify()`` fails the docs build when

* a file appears in ``tools/`` that the table has no group for, so a new script is a
  deliberate act rather than an omission, and
* a script has no purpose line to show — a shell script whose only comment is the SPDX
  header is not documented, and an undocumented script is not a usable feature.

The "where the guide explains it" column is a coverage report, not a gate: several
scripts are documented only here. It never cites a chapter the published build does not
ship, and two scripts whose own docstring names native-module machinery get a shorter
purpose on that build — the docstring stays accurate for the internal one, because the
word that cannot be published there is the word that cannot be published here.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.statemachine import StringList
from sphinx.errors import ExtensionError

from minihub_catalog_tables import _table
from omit_elf_plugin_docs import HIDE_L3, _DOCNAMES, _L3_MARKERS as _TIER_WORDS

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
DOCS = ROOT / "docs"

# Purposes used only when this build publishes no native-module chapters. Kept in
# English like every other purpose, because purposes are quoted from the tools.
PUBLISHED_PURPOSE = {
    "build_store_modules.py":
        "Discover and pack the Store modules in a tree into deployable .mhub packages.",
    "pack_store_module.py":
        "Pack a plugin source directory (or MINIHUB_PLUGIN_DIRS) into a Store .mhub.",
}

# One row per group, in the order the page shows them. The value is the group title as
# it appears on the page.
GROUPS = {
    "Build a product": ["build_firmware.py", "build_vibe_mode.py", "sync_host_manifest.py",
                        "compose_sdkconfig.py", "apply_sdkconfig_variant.sh",
                        "apply_component_patches.sh", "gen_flash_binaries.py"],
    "Pack and publish": ["new_store_module.py", "pack_store_module.py",
                         "build_store_modules.py", "build_plugin_standalone.py",
                         "sync_store_catalog.py", "gen_deploy_index.py",
                         "publish_firmware.py", "deploy.sh", "store_server.py",
                         "store_paths.py", "product_assets.py", "minify_web_assets.py",
                         "minify_lcd_ui_assets.py",
                         "generate_devices_property_from_json.py"],
    "The guide itself": ["build_docs.sh", "preview_docs.py", "publish_docs.sh"],
    "Check and gate": ["check_plugin_sdk.py", "check_module_so_layout.py",
                       "gen_abi_contract.py", "gen_host_export_symbols.py",
                       "abi_contract_lib.py", "test_deploy_index.py",
                       "test_build_plugin_standalone.py", "test_doc_figures.py",
                       "test_hide_l3_strip.py", "test_ble_mesh_map.py",
                       "test_capability_map.py", "gen_lcd_ui_contract.py",
                       "gen_lcd_i18n.py", "test_lcd_recipe.py",
                       "test_lcd_ui_contract.py", "test_lcd_ui_editor.py"],
    "Work with a running Hub": ["dev_sync.py", "athena_hook.py"],
    "For plugin authors": ["minihub_plugin_mcp.py"],
}


def _scripts() -> list[Path]:
    return sorted(p for p in TOOLS.iterdir()
                  if p.is_file() and p.suffix in (".py", ".sh"))


def _raw_purpose(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".py":
        try:
            doc = ast.get_docstring(ast.parse(text)) or ""
        except SyntaxError as exc:
            raise ExtensionError(f"minihub-tools: cannot parse {path.name}: {exc}")
        lines = [ln.strip() for ln in doc.splitlines() if ln.strip()]
        return lines[0] if lines else ""
    out = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("#"):
            if stripped:
                break
            continue
        body = stripped.lstrip("#").strip()
        if not body or body.startswith("SPDX") or body.startswith("!/"):
            continue
        if body.lower().startswith("usage:"):
            break
        out.append(body)
        if len(" ".join(out)) > 40:
            break
    return " ".join(out)


def _purpose(path: Path) -> str:
    """The script's own first line, safe to drop into an RST table cell.

    Sphinx typography turns ``--out`` into an en dash, which would leave the table
    advertising a flag that does not exist; the escapes stop that.
    """
    text = (PUBLISHED_PURPOSE[path.name] if HIDE_L3 and path.name in PUBLISHED_PURPOSE
            else _raw_purpose(path))
    return text.replace("--", "\\-\\-")


_usage_cache: dict[Path, list[str]] = {}


def _usage(path: Path, zh: bool) -> list[str]:
    """How to invoke the script, taken from how it actually reads its arguments.

    Argparse scripts get their long options. Scripts that parse ``sys.argv`` by hand get
    their own ``Usage:`` lines, verbatim, because a table that invents flags is worse than
    none — ``--board`` still appears in ``build_firmware.py``, but only in the branch that
    refuses it. A file with neither is a library or a test, and is said so.
    """
    if path in _usage_cache:
        return _usage_cache[path]
    text = path.read_text(encoding="utf-8", errors="replace")
    opts = sorted(set(re.findall(r"add_argument\(\s*[\"'](--[\w-]+)[\"']", text)))
    if opts:
        out = [", ".join(f"``{o}``" for o in opts[:6]) + (" …" if len(opts) > 6 else "")]
    else:
        labels = sorted(set(re.findall(r"^\s+(-{1,2}[\w-]+)\|?\)", text, re.M)))
        usage = _usage_lines(text)
        if labels and path.suffix == ".sh":
            out = [", ".join(f"``{o}``" for o in labels)]
        elif usage:
            out = [f"``{line}``" for line in usage]
        elif re.search(r'^if __name__ == [\'"]__main__[\'"]', text, re.M):
            out = [("直接运行 " if zh else "run directly ")
                   + f"``python3 tools/{path.name}``"]
        else:
            out = [("没有命令行入口，被其他脚本 import" if zh
                    else "no command line; imported by other scripts")]
    _usage_cache[path] = out
    return out


def _usage_lines(text: str) -> list[str]:
    """Up to two lines after the first ``Usage:`` marker, comments included."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if not re.match(r"\s*(?:#[\s]*)?[Uu]sage:", line):
            continue
        out = []
        for nxt in lines[i + 1:]:
            body = nxt.strip().lstrip("#").strip()
            if not body:
                break
            out.append(body)
            if len(out) == 2:
                break
        return out
    return []


def _flags(path: Path) -> list[str]:
    """Long options a script accepts, from its argparse declarations."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return sorted(set(re.findall(r"add_argument\(\s*[\"'](--[\w-]+)[\"']", text)))


def _guide_pages(name: str, lang_dir: Path) -> list[str]:
    """Guide pages (relative docnames) that mention this script.

    The tools page itself is excluded — it names every script by construction — and so
    is any chapter this build does not publish.
    """
    stem = Path(name).name
    hits = []
    for path in sorted(lang_dir.rglob("*.rst")):
        docname = str(path.relative_to(lang_dir).with_suffix("")).replace("\\", "/")
        if docname == "tools" or (HIDE_L3 and docname in _DOCNAMES):
            continue
        if stem in path.read_text(encoding="utf-8", errors="replace"):
            hits.append(docname)
    return hits


def rows(lang: str) -> list[tuple[str, list[str]]]:
    """(group, script names) in page order."""
    return [(g, list(names)) for g, names in GROUPS.items()]


def verify() -> list[str]:
    problems = []
    listed = {n for names in GROUPS.values() for n in names}
    present = {p.name for p in _scripts()}
    for missing in sorted(present - listed):
        problems.append(f"tools/{missing} is not in any group on the page")
    for ghost in sorted(listed - present):
        problems.append(f"the page lists tools/{ghost}, which no longer exists")
    for path in _scripts():
        if not _purpose(path):
            problems.append(f"{path.name} has no purpose line to show")
    for name in PUBLISHED_PURPOSE:
        if name not in present:
            problems.append(f"a published-purpose override names {name}, which is gone")
    if HIDE_L3:
        for path in _scripts():
            kept = _raw_purpose(path)
            if any(marker in kept for marker in _TIER_WORDS) and path.name not in PUBLISHED_PURPOSE:
                problems.append(
                    f"{path.name}'s docstring says {kept[:60]!r}, which this build does "
                    "not publish; add a PUBLISHED_PURPOSE line")
    return problems


def _table_rows(group: str, lang: str) -> list[str]:
    problems = verify()
    if problems:
        raise ExtensionError("minihub-tools is out of date:\n  " + "\n  ".join(problems))
    zh = lang.startswith("zh")
    lang_dir = DOCS / ("zh_CN" if zh else "en")
    header = (["脚本", "做什么", "用法", "指南里在哪里讲过"] if zh
              else ["Script", "What it does", "Usage", "Where the guide explains it"])
    widths = [20, 32, 28, 20]
    out = []
    for name in dict.fromkeys(GROUPS[group]):
        path = TOOLS / name
        if not path.is_file():
            raise ExtensionError(f"minihub-tools: {name} grouped but missing")
        pages = _guide_pages(name, lang_dir)
        cited = ", ".join(f":doc:`{p}`" for p in pages[:2])
        out.append([f"``{name}``", _purpose(path), _usage(path, zh),
                    (cited + (" …" if len(pages) > 2 else "")) if pages
                    else ("仅本页" if zh else "this page only")])
    table = _table(header, widths, out)
    table[0] = f".. list-table:: {group}"
    return table


class ToolsTable(Directive):
    # Group titles are whole phrases ("Work with a running Hub"), so the argument keeps
    # its whitespace instead of being split into tokens.
    required_arguments = 1
    optional_arguments = 0
    final_argument_whitespace = True
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        group = self.arguments[0].strip()
        if group not in GROUPS:
            raise ExtensionError(
                f"minihub-tools: unknown group {group!r}; use one of "
                + ", ".join(GROUPS))
        node = nodes.Element()
        self.state.nested_parse(StringList(
            _table_rows(group, env.config.language)), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-tools", ToolsTable)
    return {"parallel_read_safe": True, "version": "1.0"}

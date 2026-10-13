# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Hide L3 native ELF plugin material from the HTML build.

``HIDE_L3`` adds the Sphinx tag ``hide_l3`` and drops the L3 chapter files.
RST keeps the text inside ``.. only:: not hide_l3``. Set ``HIDE_L3 = False``
to publish those chapters and passages again.
"""

from __future__ import annotations

from posixpath import normpath

from docutils import nodes

# Docnames relative to docs/en or docs/zh_CN (no .rst).
_DOCNAMES = {
    "plugins/develop_l3",
    "api/module_abi",
    "api/plugin_host",
    "api/plugin_sdk_minihub",
    "api/plugin_sdk_esp_idf",
    "api/plugin_sdk_cjson",
    "api/plugin_sdk_mbedtls",
    "api/plugin_sdk_psa",
    "api/plugin_sdk_lwip",
    "api/plugin_sdk_freertos",
}

_TITLES = {
    "plugins/develop_l3": {
        "en": "Develop L3 (Native) Modules",
        "zh_CN": "开发 L3（原生）模块",
    },
    "api/module_abi": {
        "en": "Module ABI (L3 native plugins)",
        "zh_CN": "模块 ABI（L3 原生插件）",
    },
    "api/plugin_host": {
        "en": "L3 Host export SDK",
        "zh_CN": "L3 Host 导出 SDK",
    },
    "api/plugin_sdk_minihub": "Plugin SDK — MiniHub",
    "api/plugin_sdk_esp_idf": "Plugin SDK — ESP-IDF",
    "api/plugin_sdk_cjson": "Plugin SDK — cJSON",
    "api/plugin_sdk_mbedtls": "Plugin SDK — mbedtls",
    "api/plugin_sdk_psa": "Plugin SDK — PSA Crypto",
    "api/plugin_sdk_lwip": "Plugin SDK — lwIP",
    "api/plugin_sdk_freertos": "Plugin SDK — FreeRTOS",
}

# Deployed HTML omits L3. Flip to False to publish the chapters and the
# ``.. only:: not hide_l3`` passages.
HIDE_L3 = True


def _on_config(app, config) -> None:
    if "hide_l3" not in app.tags:
        return
    patterns = list(config.exclude_patterns)
    for name in sorted(_DOCNAMES):
        pattern = f"{name}.rst"
        if pattern not in patterns:
            patterns.append(pattern)
    config.exclude_patterns = patterns


def _resolve(refdoc: str, target: str) -> str:
    target = target.split("#", 1)[0].strip()
    if target.endswith(".rst"):
        target = target[:-4]
    if not target:
        return ""
    if target.startswith("/"):
        return normpath(target[1:])
    parent = refdoc.rsplit("/", 1)[0] if "/" in refdoc else ""
    return normpath(f"{parent}/{target}" if parent else target)


def _title(docname: str, language: str, fallback: str) -> str:
    title = _TITLES.get(docname, fallback)
    if isinstance(title, dict):
        return title.get(language) or title.get("en") or fallback
    return title


def _on_missing(app, env, node, contnode):
    if node.get("reftype") != "doc":
        return None
    docname = _resolve(node.get("refdoc") or "", node.get("reftarget") or "")
    if docname not in _DOCNAMES:
        return None
    raw = contnode.astext()
    target = (node.get("reftarget") or "").split("#", 1)[0]
    if raw == target or raw == docname or "/" in raw:
        text = _title(docname, app.config.language, raw)
        return nodes.inline("", text)
    return contnode


_L3_MARKERS = (
    "L3",
    "module_abi",
    "plugin_host",
    "plugin_sdk",
    "MHMD",
    "dlopen",
    "dlclose",
    "MINIHUB_MODULE_ABI",
)


def _strip_generated(app, doctree, docname) -> None:
    """Drop L3 sentences that Doxygen copied into the C API pages."""
    if "hide_l3" not in app.tags:
        return
    for node in list(doctree.findall(nodes.paragraph)):
        text = node.astext()
        if not any(marker in text for marker in _L3_MARKERS):
            continue
        kept = []
        for sentence in text.split(". "):
            sentence = sentence.strip()
            if not sentence or any(marker in sentence for marker in _L3_MARKERS):
                continue
            if not sentence.endswith("."):
                sentence += "."
            kept.append(sentence)
        if not kept:
            node.parent.remove(node)
            continue
        node.clear()
        node += nodes.Text(" ".join(kept))


def setup(app):
    if HIDE_L3:
        app.tags.add("hide_l3")
    app.connect("config-inited", _on_config)
    app.connect("missing-reference", _on_missing)
    app.connect("doctree-resolved", _strip_generated)
    return {
        "parallel_read_safe": True,
        "parallel_write_safe": True,
        "version": "1.0",
    }

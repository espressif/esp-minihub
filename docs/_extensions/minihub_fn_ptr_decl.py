# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Rewrite Doxygen function-pointer declarators for Sphinx.

Doxygen emits ``esp_err_t(*) name(args)``. That is not a C declarator, so
Sphinx warns and the HTML drops the pointer. The header keeps the real
``esp_err_t (*name)(args)`` typedef; this only fixes the docs rendering.
"""

from __future__ import annotations

import re

# ret(*) qualified::name(args)  ->  ret (*qualified::name)(args)
_FN_PTR_DECL = re.compile(
    r"^(?P<ret>.+?)\s*\(\*\)\s+(?P<name>[\w:.]+)\s*(?P<args>\(.*\))\s*$",
    re.DOTALL,
)


def rewrite_fn_ptr_decl(declaration: str) -> str:
    text = " ".join(declaration.split())
    match = _FN_PTR_DECL.match(text)
    if not match:
        return declaration
    ret = match.group("ret").rstrip()
    return f"{ret} (*{match.group('name')}){match.group('args')}"


def setup(app):
    from breathe.renderer.sphinxrenderer import SphinxRenderer

    original = SphinxRenderer.handle_declaration

    def handle_declaration(self, node, declaration, *args, **kwargs):
        return original(self, node, rewrite_fn_ptr_decl(declaration), *args, **kwargs)

    SphinxRenderer.handle_declaration = handle_declaration
    return {"version": "0.1", "parallel_read_safe": True, "parallel_write_safe": True}

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Check the hide_l3 strip: paragraphs filter by sentence, bullets go whole."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docs" / "_extensions"))

from docutils import nodes  # noqa: E402
from docutils.core import publish_doctree  # noqa: E402

from omit_elf_plugin_docs import _L3_MARKERS, _strip_generated  # noqa: E402


class _App:
    tags = ["hide_l3"]


def _strip(rst: str) -> nodes.document:
    tree = publish_doctree(rst, settings_overrides={"report_level": 5, "halt_level": 6})
    _strip_generated(_App(), tree, "test")
    return tree


def _assert_no_marker(tree: nodes.document) -> None:
    for marker in _L3_MARKERS:
        assert marker not in tree.astext(), tree.pformat()


def main() -> int:
    # A paragraph loses only the sentence that carries the marker.
    tree = _strip(
        "The pack writes L1 and L2 packages. A native module calls dlopen at "
        "load. Nothing else changes.\n"
    )
    text = tree.astext()
    assert "L1 and L2 packages" in text, text
    assert "Nothing else changes" in text, text
    _assert_no_marker(tree)

    # A bullet goes whole: cutting its first sentence would leave the rest of the
    # item without the label that said what it was.
    tree = _strip(
        "* **L1** -- a JSON pack.\n"
        "* **L3** -- native C++ wrapped with MHMD. For developers who need the\n"
        "  export allowlist.\n"
    )
    items = list(tree.findall(nodes.list_item))
    assert len(items) == 1, tree.pformat()
    assert "For developers who need" not in tree.astext(), tree.pformat()
    assert "L1" in items[0].astext(), items[0].pformat()

    # Chinese has no ". " separator, so the whole paragraph used to go and leave
    # an <li> with nothing in it.
    tree = _strip("* **L1** —— JSON 包。\n* **L3** —— 原生 C/C++ 编成 MHMD 加载。\n")
    items = list(tree.findall(nodes.list_item))
    assert len(items) == 1, tree.pformat()
    assert all(i.astext().strip() for i in items), tree.pformat()

    # A list that loses its only item goes with it, so no empty <ul> ships.
    tree = _strip("* An L3 module is wrapped with MHMD.\n")
    assert not list(tree.findall(nodes.bullet_list)), tree.pformat()
    _assert_no_marker(tree)

    # The tier also appears in lower case -- as the project skill name and the
    # chapter file -- and those spellings carry the same material.
    tree = _strip(
        "Packing is shared by every tier. The skill minihub-plugin-l3 wraps the "
        "native one, documented in develop_l3.\n"
    )
    text = tree.astext()
    assert "Packing is shared by every tier" in text, text
    _assert_no_marker(tree)

    print("test_hide_l3_strip: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

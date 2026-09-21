# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Flat language-switch links for MiniHub docs deploy.

esp-docs ``:link_to_translation:`` builds URLs like
``../../../zh_CN/<release>/<target>/page.html`` (docs.espressif.com layout).

MiniHub serves HTML as::

    /en/<page>.html
    /zh_CN/<page>.html

(``preview_docs.py``, ``ci_artifacts/docs/``). This transform runs before the
stock esp-docs one and emits sibling-language relative links.
"""

from __future__ import annotations

from docutils import nodes
from sphinx.transforms.post_transforms import SphinxPostTransform

from esp_docs.esp_extensions.link_roles import translation_link


class MinihubFlatTranslationLinkTransform(SphinxPostTransform):
    # Before esp-docs TranslationLinkNodeTransform (priority 0).
    default_priority = -1

    def run(self, **kwargs):
        for node in list(self.document.traverse(translation_link)):
            if "html" not in self.app.builder.name:
                node.replace_self([])
                continue
            rawtext, text, options = node["expr"]
            language, link_text = text.split(":", 1)
            env = self.document.settings.env
            docname = env.docname
            doc_path = env.doc2path(docname, False)
            return_path = "../" * doc_path.count("/")
            # From /en/api/foo.html → ../../zh_CN/api/foo.html → /zh_CN/api/foo.html
            url = f"{return_path}../{language}/{docname}.html".replace("\\", "/")
            node.replace_self(nodes.reference(rawtext, link_text, refuri=url, **options))


def setup(app):
    app.add_post_transform(MinihubFlatTranslationLinkTransform)
    return {
        "parallel_read_safe": True,
        "parallel_write_safe": True,
        "version": "1.0",
    }

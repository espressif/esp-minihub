# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Language-switch links for the MiniHub docs deploy layout.

MiniHub publishes HTML as::

    /<lang>/<version>/<page>.html

(``tools/publish_docs.sh``, ``.gitlab/ci/deploy_docs.yml``), for example
``/en/latest/architecture.html``. A switch has to climb out of both
``<version>`` and ``<lang>`` and re-enter at the same version::

    ../../zh_CN/latest/architecture.html        (top-level page)
    ../../../zh_CN/latest/api/host_rest.html   (page under api/)

The stock esp-docs ``:link_to_translation:`` transform inserts a ``<target>``
segment; MiniHub has no target segment. This transform runs before it
(priority -1) and emits the deployed path. ``conf_common.py`` stamps ``release``
from ``MINIHUB_DOCS_VERSION``, the same value the deploy job publishes under.
"""

from __future__ import annotations

from docutils import nodes
from sphinx.transforms.post_transforms import SphinxPostTransform

from esp_docs.esp_extensions.link_roles import translation_link


class MinihubTranslationLinkTransform(SphinxPostTransform):
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
            # One ".." for <version>, one for <lang>, plus one per docname dir.
            return_path = "../" * (2 + doc_path.count("/"))
            version = env.config.release or "latest"
            url = f"{return_path}{language}/{version}/{docname}.html".replace("\\", "/")
            node.replace_self(nodes.reference(rawtext, link_text, refuri=url, **options))


def setup(app):
    app.add_post_transform(MinihubTranslationLinkTransform)
    return {
        "parallel_read_safe": True,
        "parallel_write_safe": True,
        "version": "1.0",
    }

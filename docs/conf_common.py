# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Shared Sphinx / esp-docs settings (mirrors espressif/esp-matter docs layout).

from esp_docs.conf_docs import *  # noqa: F401,F403

import os
import sys

_DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_DOCS_DIR, "_extensions"))

# Pack Host type-catalog CSV / SVGs / demo devices.json into html_static_path.
from stage_catalog import stage_catalog_assets, stage_module_sdk_zip  # noqa: E402
from omit_elf_plugin_docs import HIDE_L3  # noqa: E402

stage_catalog_assets()
if HIDE_L3:
    _sdk_zip = os.path.join(_DOCS_DIR, "_static", "module_sdk.zip")
    if os.path.isfile(_sdk_zip):
        os.remove(_sdk_zip)
else:
    stage_module_sdk_zip()

languages = ["en", "zh_CN"]
idf_targets = ["esp32s31"]

# C API pages: Doxygen comments in headers → Breathe via esp-docs run_doxygen.
extensions += [  # noqa: F405
    "esp_docs.esp_extensions.dummy_build_system",
    "esp_docs.esp_extensions.run_doxygen",
    "minihub_translation_links",
    "minihub_fn_ptr_decl",
    # HIDE_L3 drops native ELF chapters and ``.. only:: not hide_l3`` passages.
    "omit_elf_plugin_docs",
]

# GitLab-hosted repo (link roles still use github_* theme keys for theme compatibility).
github_repo = "ae_group/minihub/esp-minihub"

linkcheck_anchors = False
linkcheck_timeout = 30
linkcheck_retries = 3

html_context["github_user"] = "ae_group"
html_context["github_repo"] = "minihub/esp-minihub"

html_static_path = ["../_static"]
html_css_files = ["minihub_api.css"]

project_slug = "esp-minihub"
versions_url = "_static/minihub_versions.js"
pdf_file_prefix = "esp-minihub"

# Sidebar: always expand the doc tree (product/plugin children visible on every
# page). titles_only keeps in-page sections out of the nav; open a page for its
# local headings / REST anchors.
html_theme_options = {
    "collapse_navigation": False,
    "navigation_depth": 4,
    "titles_only": True,
}

latex_elements = {
    "printindex": "",
    "makeindex": "",
}

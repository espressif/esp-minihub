# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Shared Sphinx / esp-docs settings (mirrors espressif/esp-matter docs layout).

from esp_docs.conf_docs import *  # noqa: F401,F403

import os
import re
import sys
from pathlib import Path

_DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_DOCS_DIR, "_extensions"))

# Pack Host type-catalog CSV / SVGs / STANDARD_TYPES into html_static_path.
from stage_catalog import stage_catalog_assets, stage_module_sdk_zip  # noqa: E402
from stage_architecture import stage_architecture_diagram  # noqa: E402
from stage_data_flow import stage_data_flow_diagram  # noqa: E402
from minihub_catalog_tables import stage_data_model_figures  # noqa: E402
from stage_rules import stage_rules_diagram  # noqa: E402
from stage_ota_flow import stage_ota_update_diagram  # noqa: E402
from stage_net_boot import stage_net_boot_diagram  # noqa: E402
from stage_boot import stage_boot_diagram  # noqa: E402
from stage_source_grid import stage_source_grid_diagram  # noqa: E402
from stage_hero import stage_hero_banner  # noqa: E402
from omit_elf_plugin_docs import HIDE_L3  # noqa: E402

stage_catalog_assets()
stage_architecture_diagram(HIDE_L3)
stage_data_flow_diagram(HIDE_L3)
stage_data_model_figures()
stage_rules_diagram()
stage_ota_update_diagram()
stage_net_boot_diagram()
stage_boot_diagram()
stage_source_grid_diagram()
stage_hero_banner()
if HIDE_L3:
    _sdk_zip = os.path.join(_DOCS_DIR, "_static", "module_sdk.zip")
    if os.path.isfile(_sdk_zip):
        os.remove(_sdk_zip)
    # ``_sources/`` ships the raw RST of every page, including the
    # ``.. only:: not hide_l3`` passages this build is not supposed to publish.
    html_copy_source = False
    html_show_sourcelink = False
else:
    stage_module_sdk_zip()

languages = ["en", "zh_CN"]

# The theme's version switcher pre-selects the current docs by matching
# DOCUMENTATION_OPTIONS.VERSION against the names in _static/minihub_versions.js.
# esp-docs defaults `version` to `git describe`, which drifts per commit and never
# matches a published version directory, so stamp it from MINIHUB_DOCS_VERSION —
# the directory name a publish step uses (see tools/publish_docs.sh).
import os as _os  # noqa: E402
MINIHUB_DOCS_VERSION = _os.environ.get("MINIHUB_DOCS_VERSION", "latest")
version = MINIHUB_DOCS_VERSION
release = MINIHUB_DOCS_VERSION


def _product_version() -> str:
    """The same version the firmware reports, read from the Kconfig that sets it.

    The guide has to say which build it documents: without it a reader cannot tell
    whether a page matches the device in front of them.
    """
    kconfig = Path(_DOCS_DIR).parent / "cmake" / "minihub_bundle" / "Kconfig.projbuild"
    if not kconfig.is_file():
        raise SystemExit(f"docs: {kconfig} not found, cannot stamp the guide version")
    text = kconfig.read_text(encoding="utf-8")
    m = re.search(r"config MINIHUB_VERSION\b.*?^\s*default\s+\"([^\"]+)\"", text, re.S | re.M)
    if not m:
        raise SystemExit("docs: no `config MINIHUB_VERSION` default in Kconfig.projbuild")
    return m.group(1)


# ``version``/``release`` stay with esp-docs (they come from the git branch and feed
# the theme's version links). The product version is offered as a substitution so a
# page can say which build it documents.
rst_prolog = f".. |product_version| replace:: v{_product_version()}\n"

# C API pages: Doxygen comments in headers → Breathe via esp-docs run_doxygen.
extensions += [  # noqa: F405
    "esp_docs.esp_extensions.dummy_build_system",
    "esp_docs.esp_extensions.run_doxygen",
    "minihub_translation_links",
    "minihub_fn_ptr_decl",
    "minihub_catalog_tables",
    "minihub_limits",
    "minihub_routes",
    "minihub_js_api",
    "minihub_errors",
    "minihub_kconfig",
    "minihub_tools",
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

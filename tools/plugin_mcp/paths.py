# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = TOOLS_DIR.parent
TEMPLATES = REPO_ROOT / "docs" / "module_sdk" / "templates"
STORE_MODULES = REPO_ROOT / "components" / "minihub_plugins"

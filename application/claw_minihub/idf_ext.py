# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""idf.py project extension: default serial flash baud 2Mbps."""

from __future__ import annotations

import os
from typing import Any


def action_extensions(base_actions: dict, project_path: str) -> dict:
    # Click reads ESPBAUD when resolving -b/--baud. Set a project default
    # unless the user (or shell) already chose a rate.
    if "ESPBAUD" not in os.environ:
        os.environ["ESPBAUD"] = "2000000"
    return {}

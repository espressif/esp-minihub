# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import re

ID_RE = re.compile(r"^[a-z][a-z0-9_]{0,30}$")

# Host adapters an L1 pack may inject into (no new radio stacks from L1).
INJECT_ADAPTERS = (
    "mqtt_broker",
    "zigbee",
    "ble_mesh",
    "modbus",
    "matter",
    "thread",
)

TYPE_VALUES = ("module", "pack")
RUNTIME_VALUES = ("pack", "js", "native", "hybrid")

JS_FORBIDDEN = (
    "esp_wifi_",
    "esp_zb_",
    "esp_ble_mesh",
    "esp_openthread",
    "wifi_set_",
    "nvs_flash_",
)


def check_module_id(module_id: str) -> str | None:
    """Return an error string, or None if valid."""
    if not ID_RE.match(module_id.strip()):
        return (
            f"invalid id {module_id!r}: lowercase letter, then [a-z0-9_], "
            "max 31 chars, no hyphens"
        )
    return None

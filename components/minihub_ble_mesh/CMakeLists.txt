# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

set(SRCS
    "minihub_ble_mesh.c"
    "ble_mesh_map.c"
    "ble_mesh_nvs.c"
    "ble_mesh_stack.c"
    "ble_mesh_bt.c"
)

# Always require bt so include paths for esp_ble_mesh / Bluedroid are available.
# Stack code is still gated by CONFIG_MINIHUB_BLE_MESH_SDK at compile time.
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_core nvs_flash espressif__cjson esp_timer bt freertos esp_coex
)

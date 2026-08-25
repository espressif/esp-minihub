# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "minihub_ble_gatt.c"
    INCLUDE_DIRS "include"
    REQUIRES bt freertos esp_timer log
)

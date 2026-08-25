# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

set(SRCS "minihub_zigbee.c" "zigbee_map.c" "zigbee_nvs.c" "zigbee_stack.c")
set(REQ minihub_core nvs_flash espressif__cjson freertos minihub_product_params)

if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND REQ espressif__esp-zigbee-lib freertos esp_timer esp_coex)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include" "."
    REQUIRES ${REQ}
)

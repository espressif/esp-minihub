# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

set(SRCS "minihub_zigbee.c" "zigbee_map.c" "zigbee_nvs.c" "zigbee_cluster_map.c")
set(REQ minihub_core nvs_flash espressif__cjson freertos minihub_product_params minihub_ble_mesh)

if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND SRCS "zigbee_stack.c")
    list(APPEND REQ espressif__esp-zigbee-lib esp_timer esp_coex)
else()
    list(APPEND SRCS "zigbee_stack_stub.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include" "."
    REQUIRES ${REQ}
    EMBED_TXTFILES "config/zigbee_cluster_map.json"
)

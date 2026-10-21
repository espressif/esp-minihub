# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

set(_SRCS "minihub_web.c" "minihub_web_ws.c" "minihub_map_tiles.c")
if(CONFIG_MINIHUB_WEB_UI)
    list(APPEND _SRCS "web_ui.c")
endif()

set(_REQUIRES
    minihub_core minihub_config minihub_rules minihub_hub minihub_pack minihub_module minihub_store
    minihub_js
    minihub_scenes minihub_auth minihub_ota minihub_update
    minihub_net
    minihub_provision
    minihub_matter
    minihub_thread
    minihub_device_widget
    esp_http_server esp_http_client esp_netif espressif__cjson
    spi_flash app_update joltwallet__littlefs
)
if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND _REQUIRES minihub_zigbee)
endif()
if(CONFIG_MINIHUB_BLE_MESH_SDK)
    list(APPEND _REQUIRES minihub_ble_mesh)
endif()

idf_component_register(
    SRCS ${_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${_REQUIRES}
)

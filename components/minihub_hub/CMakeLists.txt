
set(_HUB_REQUIRES
    minihub_core minihub_config minihub_adapter minihub_pack minihub_module minihub_js
    minihub_rules minihub_scenes minihub_rainmaker freertos
    minihub_matter
)

if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND _HUB_REQUIRES minihub_zigbee)
endif()
if(CONFIG_MINIHUB_BLE_MESH_SDK)
    list(APPEND _HUB_REQUIRES minihub_ble_mesh)
endif()
if(CONFIG_MINIHUB_THREAD_ADAPTER)
    list(APPEND _HUB_REQUIRES minihub_thread)
endif()

idf_component_register(
    SRCS "hub_reload.c" "hub_point_remove.c"
    INCLUDE_DIRS "include"
    REQUIRES ${_HUB_REQUIRES}
)

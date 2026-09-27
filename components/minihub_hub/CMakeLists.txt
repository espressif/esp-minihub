
set(_HUB_REQUIRES
    minihub_core minihub_config minihub_adapter minihub_pack minihub_module minihub_js
    minihub_rules minihub_scenes minihub_rainmaker freertos
    minihub_matter
    # Always require minihub_thread (stub when Thread is off). Gating REQUIRES
    # on CONFIG_MINIHUB_THREAD_ADAPTER is evaluated during early expansion,
    # when Kconfig is unavailable, so hub_reload.c loses the include path
    # while sdkconfig.h still pulls in minihub_thread.h. Same as minihub_web
    # and minihub_matter.
    minihub_thread
)

if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND _HUB_REQUIRES minihub_zigbee)
endif()
if(CONFIG_MINIHUB_BLE_MESH_SDK)
    list(APPEND _HUB_REQUIRES minihub_ble_mesh)
endif()

idf_component_register(
    SRCS "hub_reload.c" "hub_point_remove.c"
    INCLUDE_DIRS "include"
    REQUIRES ${_HUB_REQUIRES}
)

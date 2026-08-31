# Aggregates MiniHub Core + MVP adapters for compile-only linkage from minihub_host main.
set(_MVP_REQUIRES
    minihub_core
    minihub_config
    minihub_adapter
    minihub_rules
    minihub_scenes
    minihub_pack
    minihub_module
    minihub_hub
    minihub_mcp_server
    minihub_net
    minihub_provision
    minihub_product_params
    minihub_rainmaker
)
if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND _MVP_REQUIRES minihub_zigbee adapter_zigbee)
endif()
if(CONFIG_MINIHUB_BLE_MESH_SDK)
    list(APPEND _MVP_REQUIRES minihub_ble_mesh adapter_ble_mesh)
endif()
if(CONFIG_MINIHUB_MATTER_ADAPTER)
    list(APPEND _MVP_REQUIRES minihub_matter adapter_matter)
endif()

idf_component_register(
    SRCS "minihub_mvp.c"
    INCLUDE_DIRS "include"
    REQUIRES ${_MVP_REQUIRES}
)

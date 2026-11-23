# When MCP is off, register a stub so IDF can scan this directory without
# REQUIRES cap_mcp_server (esp-claw submodule). Default is on.
if(NOT CONFIG_MINIHUB_MCP_SERVER)
    idf_component_register()
    return()
endif()

set(_MCP_REQUIRES
    minihub_core minihub_hub minihub_rules minihub_scenes minihub_config minihub_store
    minihub_module minihub_ota minihub_net json cap_mcp_server mcp_mdns
)

if(CONFIG_MINIHUB_ZIGBEE_SDK)
    list(APPEND _MCP_REQUIRES minihub_zigbee)
endif()
if(CONFIG_MINIHUB_BLE_MESH_SDK)
    list(APPEND _MCP_REQUIRES minihub_ble_mesh)
endif()
if(CONFIG_MINIHUB_MATTER_ADAPTER)
    list(APPEND _MCP_REQUIRES minihub_matter)
endif()

idf_component_register(
    SRCS "minihub_mcp_server.c" "hub_api_gateway.c"
    INCLUDE_DIRS "include" "."
    REQUIRES ${_MCP_REQUIRES}
)

# IDF 6.2 component manager keeps only this yml's deps (cjson), so CMake
# REQUIRES for Hub/MCP headers can be dropped. Re-link the Hub graph here.
target_link_libraries(${COMPONENT_LIB} PUBLIC
    idf::minihub_core
    idf::minihub_hub
    idf::minihub_rules
    idf::minihub_scenes
    idf::minihub_config
    idf::minihub_store
    idf::minihub_module
    idf::minihub_ota
    idf::minihub_net
    idf::json
    idf::cap_mcp_server
    idf::mcp_mdns)
if(CONFIG_MINIHUB_ZIGBEE_SDK)
    target_link_libraries(${COMPONENT_LIB} PUBLIC idf::minihub_zigbee)
endif()
if(CONFIG_MINIHUB_BLE_MESH_SDK)
    target_link_libraries(${COMPONENT_LIB} PUBLIC idf::minihub_ble_mesh)
endif()
if(CONFIG_MINIHUB_MATTER_ADAPTER)
    target_link_libraries(${COMPONENT_LIB} PUBLIC idf::minihub_matter)
endif()

# Local tool dispatch builds MCP property lists via SDK private helpers.
idf_component_get_property(mcp_sdk_dir espressif__mcp-c-sdk COMPONENT_DIR)
if(mcp_sdk_dir)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${mcp_sdk_dir}/src/priv_include")
endif()

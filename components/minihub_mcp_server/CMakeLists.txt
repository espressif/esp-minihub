set(_MCP_REQUIRES
    minihub_core minihub_hub minihub_rules minihub_config minihub_store
    minihub_module minihub_ota minihub_net json cap_mcp_server
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

# Local tool dispatch builds MCP property lists via SDK private helpers.
idf_component_get_property(mcp_sdk_dir espressif__mcp-c-sdk COMPONENT_DIR)
if(mcp_sdk_dir)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${mcp_sdk_dir}/src/priv_include")
endif()

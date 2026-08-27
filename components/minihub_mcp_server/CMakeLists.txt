idf_component_register(
    SRCS "minihub_mcp_server.c" "hub_api_gateway.c"
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_core minihub_hub minihub_rules minihub_config minihub_store
             minihub_module minihub_ota minihub_zigbee minihub_ble_mesh minihub_net json
             cap_mcp_server
)

# Local tool dispatch builds MCP property lists via SDK private helpers.
idf_component_get_property(mcp_sdk_dir espressif__mcp-c-sdk COMPONENT_DIR)
if(mcp_sdk_dir)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${mcp_sdk_dir}/src/priv_include")
endif()

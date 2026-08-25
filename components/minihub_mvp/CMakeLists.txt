# Aggregates MiniHub Core + MVP adapters for compile-only linkage from minihub_host main.
idf_component_register(
    SRCS "minihub_mvp.c"
    INCLUDE_DIRS "include"
    REQUIRES
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
        minihub_zigbee
        minihub_product_params
        minihub_rainmaker
        adapter_zigbee
)

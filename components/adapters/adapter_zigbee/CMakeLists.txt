
idf_component_register(
    SRCS "zigbee_adapter.c" "zigbee_rpc.c"
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_core minihub_adapter minihub_module minihub_zigbee minihub_ble_mesh minihub_product_params espressif__cjson
)


idf_component_register(
    SRCS
        "ble_mesh_adapter.c"
        "ble_mesh_rpc.c"
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_core minihub_adapter minihub_module minihub_ble_mesh minihub_zigbee
             minihub_product_params minihub_device_widget espressif__cjson
)

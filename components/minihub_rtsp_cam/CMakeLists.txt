idf_component_register(
    SRCS "rtsp_record.c" "rtsp_cam_rpc.c" "rtsp_relay.c"
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_usb_msc minihub_module minihub_auth minihub_core lwip freertos nvs_flash
             espressif__cjson mbedtls esp_netif esp_http_server heap
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

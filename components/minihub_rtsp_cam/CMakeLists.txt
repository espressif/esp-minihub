# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). Callers guard minihub_rtsp_cam_* with the same option.
set(_rtsp_srcs "")
if(CONFIG_MINIHUB_RTSP_CAM)
    set(_rtsp_srcs "rtsp_record.c" "rtsp_cam_rpc.c" "rtsp_relay.c")
endif()

idf_component_register(
    SRCS ${_rtsp_srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_usb_msc minihub_module minihub_auth minihub_core lwip freertos nvs_flash
             espressif__cjson mbedtls esp_netif esp_http_server heap
)

if(CONFIG_MINIHUB_RTSP_CAM)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
endif()

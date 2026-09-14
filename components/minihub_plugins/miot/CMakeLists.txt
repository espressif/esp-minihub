idf_component_register(
    SRCS
        "miot_adapter.c"
        "miio_lan.c"
        "module_entry.c"
        "module_bridge.c"
        "miot_rpc.c"
        "auth/minihub_miot_auth.c"
        "auth/miot_cloud.c"
        "auth/miot_aes128.c"
        "auth/miot_cloud_2fa.c"
        "auth/miot_sync.c"
        "auth/miot_spec.c"
        "auth/miot_http.c"
    INCLUDE_DIRS "." "auth/include"
    REQUIRES minihub_adapter minihub_core minihub_config minihub_module
             esp_http_client espressif__cjson lwip mbedtls nvs_flash freertos
             minihub_net
    PRIV_REQUIRES esp-tls
)

if(CONFIG_MINIHUB_DYNAMIC_MODULES)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fPIC)
endif()

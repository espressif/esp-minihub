
idf_component_register(
    SRCS "minihub_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client app_update minihub_core minihub_net mbedtls
)

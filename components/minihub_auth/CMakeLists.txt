
idf_component_register(
    SRCS "minihub_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_server
)

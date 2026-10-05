idf_component_register(
    SRCS "minihub_store.c" "hub_zip.c"
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_config minihub_module minihub_pack minihub_net esp_http_client esp_rom heap nvs_flash espressif__cjson
)

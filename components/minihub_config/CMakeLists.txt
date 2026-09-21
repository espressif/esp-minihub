
idf_component_register(
    SRCS "minihub_config.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson joltwallet__littlefs minihub_device_widget
    PRIV_REQUIRES esp_partition
)


idf_component_register(
    SRCS "minihub_rules.c"
    INCLUDE_DIRS "include"
    REQUIRES minihub_core espressif__cjson freertos lwip esp_timer
)

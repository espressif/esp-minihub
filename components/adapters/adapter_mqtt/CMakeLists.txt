
idf_component_register(
    SRCS "mqtt_adapter.c"
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter espressif__mqtt espressif__cjson
)

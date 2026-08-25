
idf_component_register(
    SRCS "matter_adapter.c"
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter minihub_matter espressif__cjson
)

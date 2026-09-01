
idf_component_register(
    SRCS "matter_adapter.c" "matter_rpc.c"
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter minihub_module minihub_matter espressif__cjson
)

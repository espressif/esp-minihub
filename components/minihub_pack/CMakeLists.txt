idf_component_register(
    SRCS "minihub_pack.c"
    INCLUDE_DIRS "include"
    REQUIRES minihub_core minihub_config minihub_module espressif__cjson
)

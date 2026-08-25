
idf_component_register(
    SRCS "hub_reload.c"
    INCLUDE_DIRS "include"
    REQUIRES minihub_core minihub_config minihub_adapter minihub_pack minihub_module minihub_js minihub_rules minihub_scenes minihub_rainmaker freertos
)

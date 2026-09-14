# Do not put esp_rainmaker in REQUIRES here. Early requirement expansion can
# run before Kconfig; a CONFIG-gated REQUIRES left bridge sources without the
# package. Component Manager injects espressif/esp_rainmaker from
# idf_component.yml when CONFIG_MINIHUB_RAINMAKER=y. optional_requires is a
# no-op when that package is not in the build (default n / Mosaic).
set(_rmaker_srcs rainmaker_stub.c)
set(_rmaker_requires minihub_core espressif__cjson minihub_ota esp_event nvs_flash esp_hw_support)

if(CONFIG_MINIHUB_RAINMAKER)
    set(_rmaker_srcs rainmaker_node.c rainmaker_bridge.c)
endif()

idf_component_register(
    SRCS ${_rmaker_srcs}
    INCLUDE_DIRS include
    REQUIRES ${_rmaker_requires}
)

idf_component_optional_requires(PUBLIC esp_rainmaker)

if(CONFIG_MINIHUB_RAINMAKER)
    message(STATUS "minihub_rainmaker: RainMaker bridge (esp_rainmaker from component manager)")
else()
    message(STATUS "minihub_rainmaker: stub only")
endif()

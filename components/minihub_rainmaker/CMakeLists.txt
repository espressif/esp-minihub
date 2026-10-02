# Do not put esp_rainmaker in REQUIRES here. Early requirement expansion can
# run before Kconfig; a CONFIG-gated REQUIRES left bridge sources without the
# package. Component Manager injects espressif/esp_rainmaker from
# idf_component.yml when CONFIG_MINIHUB_RAINMAKER=y. optional_requires is a
# no-op when that package is not in the build (default n / Mosaic).
set(_rmaker_srcs rainmaker_stub.c)
set(_rmaker_requires minihub_core espressif__cjson minihub_ota esp_event nvs_flash esp_hw_support mbedtls)

# esp_sha_compat.c provides a weak, software esp_sha() (PSA-backed) so
# RainMaker links under soft crypto. It is always compiled (not only under
# CONFIG_MINIHUB_RAINMAKER) so the symbol exists in every build; the top-level
# CMakeLists then forces the linker to resolve it with -Wl,-u,esp_sha
# regardless of component link order. The definition is weak and carries no
# CONFIG_MBEDTLS_HARDWARE_SHA gate: that macro is mbedTLS-internal and has
# moved between IDF versions, and a build where it is defined would otherwise
# compile this file to an empty object while the IDF supplies no esp_sha() ->
# "undefined reference to esp_sha" from esp_rmaker_node_auth.c.
set(_rmaker_sha_srcs esp_sha_compat.c)

if(CONFIG_MINIHUB_RAINMAKER)
    set(_rmaker_srcs rainmaker_node.c rainmaker_bridge.c)
endif()

idf_component_register(
    SRCS ${_rmaker_srcs} ${_rmaker_sha_srcs}
    INCLUDE_DIRS include
    REQUIRES ${_rmaker_requires}
)

idf_component_optional_requires(PUBLIC esp_rainmaker)

if(CONFIG_MINIHUB_RAINMAKER)
    message(STATUS "minihub_rainmaker: RainMaker bridge (esp_rainmaker from component manager)")
else()
    message(STATUS "minihub_rainmaker: stub only")
endif()

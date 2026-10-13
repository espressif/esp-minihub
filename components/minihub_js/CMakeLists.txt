idf_component_register(
    SRCS "minihub_js.c" "minihub_js_ext.c"
    INCLUDE_DIRS "include" "."
    REQUIRES minihub_core minihub_config minihub_module minihub_adapter esp_http_client mbedtls lwip
)

if(CONFIG_MINIHUB_JS_PLUGINS)
    # Name used by IDF component manager for espressif/quickjs-ng
    idf_component_get_property(qjs_lib espressif__quickjs-ng COMPONENT_LIB)
    if(qjs_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${qjs_lib})
    endif()
endif()

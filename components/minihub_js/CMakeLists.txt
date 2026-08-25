idf_component_register(
    SRCS "minihub_js.c"
    INCLUDE_DIRS "include"
    REQUIRES minihub_core minihub_config esp_http_client
)

if(CONFIG_MINIHUB_JS_PLUGINS)
    # Name used by IDF component manager for espressif/quickjs-ng
    idf_component_get_property(qjs_lib espressif__quickjs-ng COMPONENT_LIB)
    if(qjs_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${qjs_lib})
    endif()
endif()

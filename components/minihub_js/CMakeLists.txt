idf_component_register(
    SRCS
        "minihub_js.c"
        "minihub_js_crypto.c"
        "minihub_js_net.c"
        "minihub_js_ble.c"
        "minihub_js_mdns.c"
        "minihub_js_mqtt.c"
    INCLUDE_DIRS "include" "."
    # NOTE: do NOT put REQUIRES inside a CONFIG_ guard — IDF resolves this list
    # before Kconfig, and a conditional here desyncs incremental builds.
    # minihub_mqtt_broker supplies minihub_mqtt.h + the loopback client; when
    # CONFIG_MINIHUB_MQTT_ADAPTER is off that source is omitted and the
    # component may be INTERFACE-only, so this stays valid.
    REQUIRES minihub_core minihub_config minihub_module minihub_adapter esp_http_client mbedtls lwip minihub_ble_gatt mdns minihub_mqtt_broker
)

if(CONFIG_MINIHUB_JS_PLUGINS)
    # Name used by IDF component manager for espressif/quickjs-ng
    idf_component_get_property(qjs_lib espressif__quickjs-ng COMPONENT_LIB)
    if(qjs_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${qjs_lib})
    endif()
endif()

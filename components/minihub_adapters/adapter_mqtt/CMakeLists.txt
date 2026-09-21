# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). espressif/mqtt comes from idf_component.yml when the adapter is on.
set(_mqtt_srcs "")
if(CONFIG_MINIHUB_MQTT_ADAPTER)
    set(_mqtt_srcs "mqtt_adapter.c")
endif()

idf_component_register(
    SRCS ${_mqtt_srcs}
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter espressif__cjson
)

# Empty SRCS (CONFIG off) makes an INTERFACE lib; PUBLIC link is illegal then.
if(CONFIG_MINIHUB_MQTT_ADAPTER)
    idf_component_optional_requires(PUBLIC espressif__mqtt)
endif()

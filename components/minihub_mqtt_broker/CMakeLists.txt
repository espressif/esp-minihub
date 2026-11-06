# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). espressif/mosquitto comes from idf_component.yml when the broker is on.
set(_mqtt_broker_srcs "")
if(CONFIG_MINIHUB_MQTT_BROKER)
    set(_mqtt_broker_srcs "minihub_mqtt_broker.c")
endif()

idf_component_register(
    SRCS ${_mqtt_broker_srcs}
    INCLUDE_DIRS "include"
    REQUIRES minihub_core minihub_config espressif__cjson esp_timer
)

# Empty SRCS (CONFIG off) makes an INTERFACE lib; PUBLIC link is illegal then.
if(CONFIG_MINIHUB_MQTT_BROKER)
    idf_component_optional_requires(PUBLIC espressif__mosquitto)
endif()

# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). espressif/mosquitto and espressif/mqtt come from idf_component.yml.
set(_mqtt_broker_srcs "")
if(CONFIG_MINIHUB_MQTT_BROKER)
    list(APPEND _mqtt_broker_srcs "minihub_mqtt_broker.c")
endif()
if(CONFIG_MINIHUB_MQTT_ADAPTER)
    list(APPEND _mqtt_broker_srcs "mqtt_adapter.c")
endif()

idf_component_register(
    SRCS ${_mqtt_broker_srcs}
    INCLUDE_DIRS "include"
    REQUIRES minihub_core minihub_config minihub_adapter espressif__cjson esp_timer
)

# Empty SRCS (CONFIG off) makes an INTERFACE lib; PUBLIC link is illegal then.
if(CONFIG_MINIHUB_MQTT_BROKER)
    idf_component_optional_requires(PUBLIC espressif__mosquitto)
endif()
if(CONFIG_MINIHUB_MQTT_ADAPTER)
    idf_component_optional_requires(PUBLIC espressif__mqtt)
endif()

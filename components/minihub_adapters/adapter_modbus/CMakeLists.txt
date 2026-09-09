# Kconfig may gate SRCS but never REQUIRES: early requirement expansion reads
# this file in a CMake pass that has no CONFIG_*, so a gated REQUIRES leaves
# the second pass without headers. esp-modbus comes from idf_component.yml
# when CONFIG_MINIHUB_MODBUS=y; optional_requires is a no-op when it is off.
set(_modbus_srcs "")
if(CONFIG_MINIHUB_MODBUS)
    set(_modbus_srcs "modbus_adapter.c" "modbus_map.c")
endif()

idf_component_register(
    SRCS ${_modbus_srcs}
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter espressif__cjson esp_netif driver
)

idf_component_optional_requires(PUBLIC espressif__esp-modbus)

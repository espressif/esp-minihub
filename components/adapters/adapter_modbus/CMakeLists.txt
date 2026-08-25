# Always declare deps: early CMake expansion may not see CONFIG_MINIHUB_MODBUS
# (same pattern as minihub_rainmaker). Runtime/code path is gated by Kconfig.
set(srcs "modbus_adapter.c" "modbus_map.c")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter espressif__cjson espressif__esp-modbus esp_netif driver
)

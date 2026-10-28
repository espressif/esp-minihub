# Soft dependency: Wi-Fi provisioning (SoftAP only) + factory reset.
# Provisioning code is linked only when CONFIG_MINIHUB_WIFI_PROVISIONING=y;
# SoftAP does not enable Bluetooth.

set(srcs "minihub_provision.c" "minihub_factory_reset.c")
# network_provisioning always listed: Kconfig may not be visible when this
# CMakeLists is first evaluated; the SoftAP manager code is still ifdef'd in the .c.
set(reqs nvs_flash esp_wifi esp_event minihub_core esp_driver_gpio espressif__network_provisioning)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
    PRIV_REQUIRES minihub_config minihub_store
)

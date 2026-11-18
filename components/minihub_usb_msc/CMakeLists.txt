# Always compile: when CONFIG_MINIHUB_USB_MSC=n the .c provides stubs so
# dependents (minihub_vibe) can link unconditionally. Kconfig must never gate
# REQUIRES (early requirement expansion has no CONFIG_*).
idf_component_register(
    SRCS "minihub_usb_msc.c"
    INCLUDE_DIRS "include"
    REQUIRES usb fatfs freertos esp_timer esp_system esp_blockdev
)

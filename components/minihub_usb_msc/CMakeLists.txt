# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). Callers guard minihub_usb_msc_* with the same option.
set(_usb_msc_srcs "")
if(CONFIG_MINIHUB_USB_MSC)
    set(_usb_msc_srcs "minihub_usb_msc.c")
endif()

idf_component_register(
    SRCS ${_usb_msc_srcs}
    INCLUDE_DIRS "include"
    REQUIRES usb fatfs freertos esp_timer esp_system
)

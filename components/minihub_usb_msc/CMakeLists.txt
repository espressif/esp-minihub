idf_component_register(
    SRCS "minihub_usb_msc.c"
    INCLUDE_DIRS "include"
    REQUIRES usb fatfs freertos esp_timer esp_system
)

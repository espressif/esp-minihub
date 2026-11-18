# Share Host-component sources without registering the whole components/ tree.
# stage.c is Host-only: Vibe Mode never downloads or arms a pending flag.
get_filename_component(VIBE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

idf_component_register(
    SRCS
        "vibe_mode_main.c"
        "${VIBE_ROOT}/minihub_vibe_media.c"
        "${VIBE_ROOT}/minihub_vibe_state.c"
        "${VIBE_ROOT}/minihub_vibe_apply.c"
    INCLUDE_DIRS "." "${VIBE_ROOT}/include"
    REQUIRES
        nvs_flash
        app_update
        esp_partition
        mbedtls
        vfs
        sdmmc
        esp_blockdev
        joltwallet__littlefs
        espressif__usb_host_msc
)

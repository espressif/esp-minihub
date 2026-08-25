# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_MINIHUB_LCD_UI)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml")
        message(FATAL_ERROR
            "MINIHUB_LCD_UI is enabled but esp-brookesia deps are missing.\n"
            "  Run: ./tools/apply_sdkconfig_variant.sh lcd && idf.py fullclean build")
    endif()
    idf_component_register(
        SRCS "minihub_lcd.c" "minihub_lcd_app.cpp" "minihub_lcd_module_auth.cpp"
        INCLUDE_DIRS "include"
        REQUIRES minihub_core minihub_pack minihub_module minihub_web minihub_hub
        PRIV_REQUIRES espressif__esp_lvgl_adapter
                 espressif__brookesia_lib_utils espressif__brookesia_hal_adaptor espressif__brookesia_hal_boards espressif__brookesia_service_manager espressif__brookesia_service_helper espressif__brookesia_service_display espressif__brookesia_gui_interface espressif__brookesia_gui_lvgl lvgl__lvgl
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_pack/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_module/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_hub/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_adapter/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_web/include"
    )
else()
    idf_component_register(
        SRCS "minihub_lcd.c"
        INCLUDE_DIRS "include"
        REQUIRES minihub_core
    )
endif()

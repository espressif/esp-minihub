# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_MINIHUB_LCD_UI)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml")
        message(FATAL_ERROR
            "MINIHUB_LCD_UI is enabled but esp-brookesia deps are missing.\n"
            "  Run: ./tools/apply_sdkconfig_variant.sh lcd && idf.py fullclean build")
    endif()
    idf_component_register(
        SRCS "minihub_lcd.c" "minihub_lcd_app.cpp" "minihub_lcd_module_auth.cpp"
             "minihub_lcd_i18n.cpp"
        INCLUDE_DIRS "include"
        REQUIRES minihub_core minihub_pack minihub_module minihub_web minihub_hub
        PRIV_REQUIRES espressif__esp_lvgl_adapter minihub_net
                 espressif__brookesia_lib_utils espressif__brookesia_hal_adaptor espressif__brookesia_service_manager espressif__brookesia_service_helper espressif__brookesia_service_display espressif__brookesia_gui_interface espressif__brookesia_gui_lvgl lvgl__lvgl
    )
    # ui_contract.json is the single source for view paths and action names;
    # a drifted JSON UI now fails the contract test instead of warning at runtime.
    set(LCD_CONTRACT_GEN ${CMAKE_CURRENT_BINARY_DIR}/include/ui_contract.gen.h)
    add_custom_command(
        OUTPUT ${LCD_CONTRACT_GEN}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_lcd_ui_contract.py
                --out ${LCD_CONTRACT_GEN}
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/ui_contract.json
                ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_lcd_ui_contract.py
        COMMENT "Generating LCD UI contract header"
    )
    add_custom_target(minihub_lcd_contract DEPENDS ${LCD_CONTRACT_GEN})
    add_dependencies(${COMPONENT_LIB} minihub_lcd_contract)
    # ui/i18n/strings.json is the single source for both the Brookesia language
    # constants (checked in, verified by gen_lcd_i18n.py --check) and the
    # firmware-side lcd_tr() dictionary. Generate the header into the build dir
    # so the checked-in tree stays clean.
    set(LCD_I18N_GEN ${CMAKE_CURRENT_BINARY_DIR}/include/lcd_strings.gen.h)
    add_custom_command(
        OUTPUT ${LCD_I18N_GEN}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_lcd_i18n.py
                --header ${LCD_I18N_GEN}
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/ui/i18n/strings.json
                ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_lcd_i18n.py
        COMMENT "Generating LCD i18n strings header"
    )
    add_custom_target(minihub_lcd_i18n DEPENDS ${LCD_I18N_GEN})
    add_dependencies(${COMPONENT_LIB} minihub_lcd_i18n)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_pack/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_module/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_hub/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_adapter/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_web/include"
        # minihub_ota is a main component whose public REQUIRES edge is dropped
        # by IDF's cycle resolution (ota -> net -> ... -> store -> lcd). We only
        # read minihub_ota_is_busy() from app.cpp, so an include path is enough;
        # do not add a component dependency edge back into the cycle.
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_ota/include"
        # Same cycle-resolution situation as minihub_ota: only an include path
        # for mask / device string constants used by the System OTA target UI.
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_vibe/include"
        # Same cycle-resolution situation as minihub_ota above: a REQUIRES edge
        # from minihub_lcd into these components is dropped by IDF, so only the
        # include path is added here. The symbols themselves are already in the
        # final link closure through minihub_web / minihub_store.
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_config/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_rules/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_scenes/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_net/include"
        # minihub_store REQUIRES minihub_lcd, so a REQUIRES edge back from
        # minihub_lcd would be a cycle and gets dropped by IDF. Only the include
        # path is added; the store symbols are in the final link closure via
        # minihub_web -> minihub_store.
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_store/include"
        "${CMAKE_CURRENT_LIST_DIR}/../minihub_update/include"
    )
else()
    idf_component_register(
        SRCS "minihub_lcd.c"
        INCLUDE_DIRS "include"
        REQUIRES minihub_core
    )
endif()

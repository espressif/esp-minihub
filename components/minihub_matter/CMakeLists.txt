# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

set(_matter_requires espressif__cjson nvs_flash minihub_config)

# Real commissioner needs esp-matter on EXTRA_COMPONENT_DIRS (root CMakeLists)
# and MINIHUB_BUILD_MATTER=1. CONFIG_MINIHUB_MATTER_ADAPTER=y without that
# is a hard error — do not silently fall back to stub.
set(_matter_real 0)
if(DEFINED ENV{MINIHUB_BUILD_MATTER} AND "$ENV{MINIHUB_BUILD_MATTER}" STREQUAL "1")
    set(_matter_real 1)
endif()

if(CONFIG_MINIHUB_MATTER_ADAPTER AND NOT _matter_real)
    message(FATAL_ERROR
        "CONFIG_MINIHUB_MATTER_ADAPTER=y requires a Matter build "
        "(MINIHUB_BUILD_MATTER=1 + ESP_MATTER_PATH). "
        "Use: ./tools/build_firmware.py … matter build "
        "or clear CONFIG_MINIHUB_MATTER_ADAPTER for stub-only builds.")
endif()

set(_matter_srcs matter_sync.c)
if(_matter_real)
    list(APPEND _matter_srcs matter_ctrl.cpp matter_wifi_bootstrap.c)
    list(APPEND _matter_requires esp_matter esp_matter_controller chip minihub_provision)
else()
    list(APPEND _matter_srcs matter_stub.c)
endif()

idf_component_register(
    SRCS ${_matter_srcs}
    INCLUDE_DIRS include
    REQUIRES ${_matter_requires}
)

if(_matter_real)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-format-nonliteral -Wno-format-security)
    # Only needed when Matter shares MiniHub Wi-Fi STA init with CHIP.
    if(CONFIG_MINIHUB_NET_WIFI_STA)
        target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")
    endif()
endif()

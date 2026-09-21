# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

# Do NOT gate REQUIRES/SRCS on CONFIG_* here. Component CMakeLists is evaluated
# during early requirement expansion when Kconfig values are unavailable; that
# leaves commissioner sources (second pass) without esp_matter/minihub_provision
# in REQUIRES (first pass). Same pattern as minihub_rainmaker.
set(_matter_requires espressif__cjson nvs_flash minihub_config)

set(_matter_real 0)
if(DEFINED ENV{ESP_MATTER_PATH} AND NOT "$ENV{MINIHUB_BUILD_MATTER}" STREQUAL "0")
    # ENV=1 from build_firmware, or unset during board-manager cmake re-entry.
    # Explicit ENV=0 → stub even when sdkconfig has MATTER_ADAPTER=y.
    set(_matter_real 1)
endif()

set(_matter_srcs matter_sync.c)
if(_matter_real)
    list(APPEND _matter_srcs matter_ctrl.cpp matter_wifi_bootstrap.c matter_ota.cpp)
    # Always REQUIRES minihub_thread (stub when Thread off). Conditioning on
    # CONFIG_MINIHUB_THREAD_ADAPTER desyncs with sdkconfig and breaks
    # #include "minihub_thread.h" in matter_ctrl.cpp.
    list(APPEND _matter_requires
        esp_matter esp_matter_controller chip minihub_provision esp_wifi minihub_thread
        minihub_core esp_http_server)
    message(STATUS "minihub_matter: linking esp-matter commissioner")
else()
    list(APPEND _matter_srcs matter_stub.c)
    message(STATUS "minihub_matter: stub only")
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

# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

# Always REQUIRES the OpenThread BR deps. Conditioning REQUIRES on
# CONFIG_MINIHUB_THREAD_ADAPTER alone desyncs with sdkconfig after
# incremental builds (compile thread_br.c with stub include paths →
# missing esp_event.h / esp_coexist.h / …). Stub sources do not call
# into these components; include/link cost is only when referenced.

if(CONFIG_MINIHUB_THREAD_ADAPTER)
    set(SRCS "thread_br.c")
else()
    set(SRCS "thread_stub.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include" "."
    REQUIRES
        minihub_config
        espressif__cjson
        nvs_flash
        openthread
        esp_netif
        esp_event
        esp_coex
        esp_wifi
        mdns
)

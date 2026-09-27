# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*).
set(_wifi_srcs "")
if(CONFIG_MINIHUB_NET_WIFI_STA)
    set(_wifi_srcs "wifi_adapter.c")
endif()

idf_component_register(
    SRCS ${_wifi_srcs}
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter minihub_net
)

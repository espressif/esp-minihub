# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

# Kconfig may gate SRCS but never REQUIRES (early requirement expansion has no
# CONFIG_*). thread_adapter is referenced only under the same guard.
set(_thread_srcs "")
if(CONFIG_MINIHUB_THREAD_ADAPTER)
    set(_thread_srcs "thread_adapter.c" "thread_rpc.c")
endif()

idf_component_register(
    SRCS ${_thread_srcs}
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter minihub_module minihub_thread espressif__cjson
)

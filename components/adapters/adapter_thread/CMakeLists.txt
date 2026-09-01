# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "thread_adapter.c" "thread_rpc.c"
    INCLUDE_DIRS "."
    REQUIRES minihub_core minihub_adapter minihub_module minihub_thread espressif__cjson
)

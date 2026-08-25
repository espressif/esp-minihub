# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "product_params.c"
    INCLUDE_DIRS "include"
    REQUIRES minihub_core espressif__cjson
)

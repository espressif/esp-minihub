/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "minihub_module_abi.h"

void minihub_miot_module_set_host(const minihub_module_host_api_t *host);
const minihub_module_host_api_t *minihub_miot_module_host(void);

/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "module_bridge.h"

#include <string.h>

static minihub_module_host_api_t s_host_storage;
static const minihub_module_host_api_t *s_host;

void minihub_miot_module_set_host(const minihub_module_host_api_t *host)
{
    if (!host) {
        memset(&s_host_storage, 0, sizeof(s_host_storage));
        s_host = NULL;
        return;
    }
    /* Copy: callers may pass a stack-local vtable (static-link adapter init). */
    s_host_storage = *host;
    s_host = &s_host_storage;
}

const minihub_module_host_api_t *minihub_miot_module_host(void)
{
    return s_host;
}

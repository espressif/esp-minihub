/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Extra Host bindings for L2 plugins: host.net.udp + host.crypto.
 */

#pragma once

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS
#include "quickjs.h"

/**
 * @brief Attach host.net and host.crypto onto an existing host object.
 */
void minihub_js_attach_net_crypto(JSContext *ctx, JSValue host);
#endif

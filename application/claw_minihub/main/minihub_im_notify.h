/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/** Subscribe to point.updated and send WeChat texts for sensor-like changes. */
esp_err_t minihub_im_notify_init(void);

/**
 * Register claw_cap + router rule so inbound WeChat messages remember chat_id
 * without patching third_party/esp-claw.
 * Call after app_claw_start().
 */
esp_err_t minihub_im_notify_register_claw_hooks(void);

/** Remember the last IM chat that messaged the hub (WeChat inbound). */
void minihub_im_notify_remember(const char *channel, const char *chat_id);

/** Queue a free-form IM text to the last WeChat chat (used by rules then=notify). */
esp_err_t minihub_im_notify_text(const char *text);

/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Account-bind capability framework for Claw IM.
 * Vendor clouds are Store plugins — not registered here.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

typedef esp_err_t (*account_bind_bind_fn)(const char *user, const char *pass,
                                         char *output, size_t output_size);
typedef esp_err_t (*account_bind_status_fn)(char *output, size_t output_size);
typedef esp_err_t (*account_bind_logout_fn)(char *output, size_t output_size);
typedef esp_err_t (*account_bind_send_fn)(const char *icode, char *output, size_t output_size);
typedef esp_err_t (*account_bind_verify_fn)(const char *ticket, char *output, size_t output_size);

typedef struct {
    const char *id;
    const char *display_name;
    bool implemented;
    account_bind_bind_fn bind;
    account_bind_status_fn status;
    account_bind_logout_fn logout;
    account_bind_send_fn send;
    account_bind_verify_fn verify;
} account_bind_provider_t;

const account_bind_provider_t *account_bind_find_provider(const char *id);
const account_bind_provider_t *account_bind_providers(void);
size_t account_bind_provider_count(void);

void account_bind_append_minihub_login_link(char *output, size_t output_size);

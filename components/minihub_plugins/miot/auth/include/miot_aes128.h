/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * In-place AES-128-CBC decrypt with PKCS7 unpad.
 * `key_len` must be 16 (extra bytes ignored). `len` must be a multiple of 16.
 */
esp_err_t miot_aes128_cbc_decrypt(const uint8_t *key, size_t key_len, const uint8_t iv[16],
                                  uint8_t *buf, size_t len, size_t *out_len);

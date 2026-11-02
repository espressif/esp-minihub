/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Software fallback implementation of esp_sha().
 *
 * The MiniHub project deliberately runs mbedTLS in software-crypto mode:
 * configs/sdkconfig/memory.defaults leaves CONFIG_MBEDTLS_HARDWARE_SHA unset
 * because the hardware SHA/AES DMA engine can hang on this chip when the RGB
 * LCD GDMA is active (sha_hal_wait_idle). The latest ESP-IDF only builds the
 * built-in esp_sha() (a hardware-SHA wrapper) when CONFIG_MBEDTLS_HARDWARE_SHA
 * is set, so with soft crypto that symbol is absent and ESP-RainMaker's
 * node-auth signing, which calls esp_sha(SHA2_256, ...), fails to link.
 *
 * This file provides a pure-software SHA implementation backed by the PSA
 * crypto API, the only SHA surface that survives in MbedTLS 4.x after the
 * legacy mbedtls/sha256.h API was removed. So the project keeps soft crypto
 * while still linking RainMaker.
 *
 * The definition is compiled unconditionally and marked __attribute__((weak)):
 * it must not depend on CONFIG_MBEDTLS_HARDWARE_SHA. That symbol is not
 * visible from this component in every build (it is an mbedTLS-internal
 * option, and its name/default has moved between IDF versions), and a build
 * where the macro happens to be defined would compile this file to an empty
 * object while the IDF side supplies no esp_sha() at all -> link failure
 * "undefined reference to esp_sha" from esp_rmaker_node_auth.c. Being weak,
 * it never collides with the real hardware implementation when the IDF does
 * provide one: a strong definition simply overrides this one.
 */
#include "psa/crypto.h"
#include "psa/crypto_sizes.h"
#include "mbedtls/platform_util.h"
#include "hal/sha_types.h"

__attribute__((weak)) void esp_sha(esp_sha_type sha_type,
                                   const unsigned char *input, size_t ilen,
                                   unsigned char *output)
{
    psa_algorithm_t alg = PSA_ALG_NONE;

    /* Map unconditionally: PSA SHA-1/SHA-256 are always available in
     * tf-psa-crypto, so relying on CONFIG_MBEDTLS_SHA1_C / SHA256_C here would
     * risk silently returning without hashing when those macros are not
     * visible to this component. */
    if (sha_type == SHA1) {
        alg = PSA_ALG_SHA_1;
    } else if (sha_type == SHA2_256) {
        alg = PSA_ALG_SHA_256;
    }

    if (alg == PSA_ALG_NONE) {
        /* Unsupported type (e.g. SHA2_224 when not built). RainMaker node-auth
         * only ever uses SHA2_256, so this is never hit in practice. */
        return;
    }

    size_t output_len = PSA_HASH_LENGTH(alg);
    size_t olen = 0;
    psa_status_t status = psa_hash_compute(alg, input, ilen,
                                           output, output_len, &olen);
    if (status != PSA_SUCCESS || olen != output_len) {
        mbedtls_platform_zeroize(output, output_len);
    }
}

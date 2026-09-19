/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file mbedtls.h
 * @brief Plugin SDK — mbedtls MD / HMAC / base64 export subset.
 *
 * Each API carries an ``@note`` with its IDF header origin.
 */

/** @brief Message-digest algorithm identifiers.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
typedef enum {
    MBEDTLS_MD_NONE = 0,
    MBEDTLS_MD_MD5 = 0x03,
    MBEDTLS_MD_RIPEMD160 = 0x04,
    MBEDTLS_MD_SHA1 = 0x05,
    MBEDTLS_MD_SHA224 = 0x08,
    MBEDTLS_MD_SHA256 = 0x09,
    MBEDTLS_MD_SHA384 = 0x0a,
    MBEDTLS_MD_SHA512 = 0x0b,
} mbedtls_md_type_t;

/** @brief Opaque message-digest info (algorithm descriptor).
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
typedef struct mbedtls_md_info_t mbedtls_md_info_t;

/** @brief MD engine selection (legacy vs PSA).
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
typedef enum {
    MBEDTLS_MD_ENGINE_LEGACY = 0,
    MBEDTLS_MD_ENGINE_PSA,
} mbedtls_md_engine_t;

/** @brief Message-digest context (Host layout includes PSA engine field).
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
#ifdef DOXYGEN
typedef struct {
    const mbedtls_md_info_t *md_info;
    mbedtls_md_engine_t engine;
    void *md_ctx;
    void *hmac_ctx;
} mbedtls_md_context_t;
#else
typedef struct mbedtls_md_context_t {
    const mbedtls_md_info_t *md_info;
    mbedtls_md_engine_t engine;
    void *md_ctx;
    void *hmac_ctx;
} mbedtls_md_context_t;
#endif

/** @brief Look up digest info by algorithm type.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t md_type);
/** @brief Initialize a message-digest context.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
void mbedtls_md_init(mbedtls_md_context_t *ctx);
/** @brief Free a message-digest context.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
void mbedtls_md_free(mbedtls_md_context_t *ctx);
/** @brief Bind digest info to context (optionally HMAC mode).
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
int mbedtls_md_setup(mbedtls_md_context_t *ctx, const mbedtls_md_info_t *md_info, int hmac);
/** @brief Start digest computation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
int mbedtls_md_starts(mbedtls_md_context_t *ctx);
/** @brief Feed input bytes into digest.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
int mbedtls_md_update(mbedtls_md_context_t *ctx, const unsigned char *input, size_t ilen);
/** @brief Finish digest and write output.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
int mbedtls_md_finish(mbedtls_md_context_t *ctx, unsigned char *output);
/** @brief One-shot digest over a buffer.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
int mbedtls_md(const mbedtls_md_info_t *md_info, const unsigned char *input, size_t ilen,
               unsigned char *output);
/** @brief One-shot HMAC over a buffer.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/md.h`` */
int mbedtls_md_hmac(const mbedtls_md_info_t *md_info, const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen, unsigned char *output);
/** @brief Base64-encode a buffer.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/base64.h`` */
int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
/** @brief Base64-decode a buffer.
 *  @note IDF: ``components/mbedtls/mbedtls/include/mbedtls/base64.h`` */
int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);

#ifdef __cplusplus
}
#endif

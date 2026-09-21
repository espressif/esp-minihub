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
 * @file psa.h
 * @brief Plugin SDK — PSA Crypto export subset.
 *
 * ``psa_key_attributes_t`` matches Host layout. Multipart cipher / derivation
 * contexts are oversized opaque buffers (Host-internal driver state).
 * Each API carries an ``@note`` with its IDF header origin.
 */

/** @brief PSA function return status.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef int32_t psa_status_t;
/** @brief PSA key type identifier.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint16_t psa_key_type_t;
/** @brief PSA key size in bits.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint16_t psa_key_bits_t;
/** @brief PSA key lifetime specifier.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint32_t psa_key_lifetime_t;
/** @brief PSA key usage flags.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint32_t psa_key_usage_t;
/** @brief PSA algorithm identifier.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint32_t psa_algorithm_t;
/** @brief PSA key identifier.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint32_t psa_key_id_t;
/** @brief Mbed TLS service key identifier alias.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef psa_key_id_t mbedtls_svc_key_id_t;
/** @brief Key derivation input step identifier.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint16_t psa_key_derivation_step_t;
/** @brief ECC curve family identifier.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef uint8_t psa_ecc_family_t;

/** @brief Successful PSA operation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_SUCCESS  ((psa_status_t)0)

/** @brief No key type.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_TYPE_NONE              ((psa_key_type_t)0x0000)
/** @brief AES key type.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_TYPE_AES               ((psa_key_type_t)0x2400)
/** @brief ECC key-pair type base.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_TYPE_ECC_KEY_PAIR_BASE ((psa_key_type_t)0x7100)
/** @brief ECC public-key type base.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY_BASE ((psa_key_type_t)0x4100)
/** @brief Build ECC key-pair type for curve.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_TYPE_ECC_KEY_PAIR(curve)  (PSA_KEY_TYPE_ECC_KEY_PAIR_BASE | (curve))
/** @brief Build ECC public-key type for curve.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(curve) (PSA_KEY_TYPE_ECC_PUBLIC_KEY_BASE | (curve))
/** @brief secp*r1 ECC family.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ECC_FAMILY_SECP_R1         ((psa_ecc_family_t)0x12)

/** @brief Volatile (RAM) key lifetime.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_LIFETIME_VOLATILE      ((psa_key_lifetime_t)0)
/** @brief Key may be used for encryption.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_USAGE_ENCRYPT          ((psa_key_usage_t)0x00000100)
/** @brief Key may be used for decryption.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_USAGE_DECRYPT          ((psa_key_usage_t)0x00000200)
/** @brief Key may be used for derivation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_USAGE_DERIVE           ((psa_key_usage_t)0x00004000)
/** @brief Key material may be exported.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_USAGE_EXPORT           ((psa_key_usage_t)0x00000001)

/** @brief SHA-256 hash algorithm.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_SHA_256                ((psa_algorithm_t)0x02000009)
/** @brief CTR block cipher mode.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_CTR                    ((psa_algorithm_t)0x04c01000)
/** @brief CCM AEAD mode.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_CCM                    ((psa_algorithm_t)0x05500100)
/** @brief GCM AEAD mode.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_GCM                    ((psa_algorithm_t)0x05500200)
/** @brief Hash algorithm field mask.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_HASH_MASK              ((psa_algorithm_t)0x000000ff)
/** @brief HKDF base algorithm.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_HKDF_BASE              ((psa_algorithm_t)0x08000100)
/** @brief Build HKDF algorithm from hash.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_HKDF(hash_alg)         (PSA_ALG_HKDF_BASE | ((hash_alg) & PSA_ALG_HASH_MASK))
/** @brief ECDH key agreement algorithm.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_ALG_ECDH                   ((psa_algorithm_t)0x09020000)

/** @brief Derivation input: secret key material.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_DERIVATION_INPUT_SECRET ((psa_key_derivation_step_t)0x0101)
/** @brief Derivation input: salt.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_DERIVATION_INPUT_SALT   ((psa_key_derivation_step_t)0x0202)
/** @brief Derivation input: info string.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_DERIVATION_INPUT_INFO   ((psa_key_derivation_step_t)0x0203)

/** @brief Key usage and algorithm policy.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef struct {
    psa_key_usage_t usage;
    psa_algorithm_t alg;
    psa_algorithm_t alg2;
} psa_key_policy_t;

/** @brief Key attributes (must match Host ``sizeof``).
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef struct psa_key_attributes_s {
    psa_key_type_t type;
    psa_key_bits_t bits;
    psa_key_lifetime_t lifetime;
    psa_key_policy_t policy;
    mbedtls_svc_key_id_t id;
} psa_key_attributes_t;

/** @brief Default-initialized key attributes.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_ATTRIBUTES_INIT  { PSA_KEY_TYPE_NONE, 0, PSA_KEY_LIFETIME_VOLATILE, { 0, 0, 0 }, 0 }

/** @brief Set key type in attributes.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
static inline void psa_set_key_type(psa_key_attributes_t *a, psa_key_type_t type)
{
    a->type = type;
}
/** @brief Set key size in bits.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
static inline void psa_set_key_bits(psa_key_attributes_t *a, size_t bits)
{
    a->bits = (psa_key_bits_t)bits;
}
/** @brief Set key usage flags.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
static inline void psa_set_key_usage_flags(psa_key_attributes_t *a, psa_key_usage_t usage)
{
    a->policy.usage = usage;
}
/** @brief Set primary key algorithm.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
static inline void psa_set_key_algorithm(psa_key_attributes_t *a, psa_algorithm_t alg)
{
    a->policy.alg = alg;
}

/** @brief Opaque multipart cipher operation state.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef struct {
    unsigned char _opaque[2048];
} psa_cipher_operation_t;

/** @brief Opaque key derivation operation state.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
typedef struct {
    unsigned char _opaque[2048];
} psa_key_derivation_operation_t;

/** @brief Initializer for cipher operation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_CIPHER_OPERATION_INIT          { { 0 } }
/** @brief Initializer for key derivation operation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
#define PSA_KEY_DERIVATION_OPERATION_INIT  { { 0 } }

/** @brief Initialize PSA Crypto subsystem.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_crypto_init(void);
/** @brief Reset key attributes to defaults.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
void psa_reset_key_attributes(psa_key_attributes_t *attributes);
/** @brief Destroy a key slot.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key);
/** @brief Import raw key material.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data,
                            size_t data_length, mbedtls_svc_key_id_t *key);
/** @brief Generate a key in a slot.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_generate_key(const psa_key_attributes_t *attributes, mbedtls_svc_key_id_t *key);
/** @brief Export public key bytes.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_export_public_key(mbedtls_svc_key_id_t key, uint8_t *data, size_t data_size,
                                   size_t *data_length);
/** @brief AEAD encrypt (one-shot).
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data,
                              size_t additional_data_length, const uint8_t *plaintext,
                              size_t plaintext_length, uint8_t *ciphertext, size_t ciphertext_size,
                              size_t *ciphertext_length);
/** @brief AEAD decrypt (one-shot).
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data,
                              size_t additional_data_length, const uint8_t *ciphertext,
                              size_t ciphertext_length, uint8_t *plaintext, size_t plaintext_size,
                              size_t *plaintext_length);
/** @brief Begin multipart encryption.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_cipher_encrypt_setup(psa_cipher_operation_t *operation, mbedtls_svc_key_id_t key,
                                      psa_algorithm_t alg);
/** @brief Begin multipart decryption.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_cipher_decrypt_setup(psa_cipher_operation_t *operation, mbedtls_svc_key_id_t key,
                                      psa_algorithm_t alg);
/** @brief Set IV for multipart cipher.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_cipher_set_iv(psa_cipher_operation_t *operation, const uint8_t *iv, size_t iv_length);
/** @brief Feed plaintext/ciphertext to multipart cipher.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_cipher_update(psa_cipher_operation_t *operation, const uint8_t *input,
                               size_t input_length, uint8_t *output, size_t output_size,
                               size_t *output_length);
/** @brief Finish multipart cipher.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_cipher_finish(psa_cipher_operation_t *operation, uint8_t *output,
                               size_t output_size, size_t *output_length);
/** @brief Abort multipart cipher.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_cipher_abort(psa_cipher_operation_t *operation);
/** @brief Begin key derivation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *operation, psa_algorithm_t alg);
/** @brief Provide derivation input bytes.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *operation,
                                            psa_key_derivation_step_t step, const uint8_t *data,
                                            size_t data_length);
/** @brief Provide derivation input key.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_key_derivation_input_key(psa_key_derivation_operation_t *operation,
                                          psa_key_derivation_step_t step, mbedtls_svc_key_id_t key);
/** @brief Output derived key material.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *operation,
                                             uint8_t *output, size_t output_length);
/** @brief Abort key derivation.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *operation);
/** @brief Raw ECDH / key-agreement output.
 *  @note IDF: ``components/mbedtls/mbedtls/include/psa/crypto.h`` */
psa_status_t psa_raw_key_agreement(psa_algorithm_t alg, mbedtls_svc_key_id_t private_key,
                                   const uint8_t *peer_key, size_t peer_key_length, uint8_t *output,
                                   size_t output_size, size_t *output_length);

#ifdef __cplusplus
}
#endif

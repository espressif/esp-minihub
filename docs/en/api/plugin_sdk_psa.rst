Plugin SDK — PSA Crypto
=======================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

PSA Crypto subset Host exports to L3 plugins (AEAD, cipher, key derivation,
ECDH). ``psa_key_attributes_t`` must match Host layout; multipart contexts are
opaque oversized buffers.

.. include-build-file:: inc/psa.inc

See also: :doc:`plugin_sdk_mbedtls`, :doc:`plugin_host`.

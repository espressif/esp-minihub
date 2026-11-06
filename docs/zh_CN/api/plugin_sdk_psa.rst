Plugin SDK — PSA Crypto
=======================

:link_to_translation:`en:[English]`

概述
----

Host 向 ELF 插件导出的 PSA Crypto 子集（AEAD、cipher、密钥派生、ECDH）。
``psa_key_attributes_t`` 须与 Host 布局一致；multipart 上下文为超大不透明缓冲。

.. include-build-file:: inc/psa.inc

另见：:doc:`plugin_sdk_mbedtls`、:doc:`plugin_host`。

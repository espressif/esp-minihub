JS plugin net and crypto host API
=================================

:link_to_translation:`zh_CN:[中文]`

These namespaces live in ``components/minihub_js/minihub_js_net.c`` and
``components/minihub_js/minihub_js_crypto.c``. The table is read from the C
bindings at build time.

.. minihub-JS-api:: net

See :doc:`js_host` for the entry point.

Methods — host.net
------------------

IPv4 UDP helpers for LAN protocols. Binary payloads use **base64**.
Implementation: ``components/minihub_js/minihub_js_net.c``.

host.net.udp.open(opts?)
~~~~~~~~~~~~~~~~~~~~~~~~

``opts.broadcast`` (bool, optional) enables ``SO_BROADCAST``.

Returns a small integer socket handle (slot index).

host.net.udp.sendto(fd, { ip, port, data_b64 })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

host.net.udp.recvfrom(fd, { timeout_ms, max_len? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Returns ``{ ip, port, data_b64 }`` or ``null`` on timeout.

host.net.udp.close(fd)
~~~~~~~~~~~~~~~~~~~~~~

Methods — host.crypto
---------------------

Digest / HMAC / AES helpers. Digests return **lowercase hex**; HMAC and AES
return **base64**. Inputs accept a string or ``{ data }`` / ``{ data_b64 }``.

* ``host.crypto.md5(input)``
* ``host.crypto.sha1(input)``
* ``host.crypto.sha256(input)``
* ``host.crypto.hmacSha256({ key_b64, data_b64 })`` → base64
* ``host.crypto.aes128CbcEncrypt({ key_b64, iv_b64, data_b64 })`` → base64 (PKCS7)
* ``host.crypto.aes128CbcDecrypt({ key_b64, iv_b64, data_b64 })`` → base64

Demo: ``examples/plugins/color_bulb_js/``.

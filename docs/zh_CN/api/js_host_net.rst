JS 插件 net 与 crypto Host API
==============================

:link_to_translation:`en:[English]`

这些命名空间实现在 ``components/minihub_js/minihub_js_net.c`` 与
``components/minihub_js/minihub_js_crypto.c``。下表在构建时从 C 绑定读出。

.. minihub-JS-api:: net

命名空间入口见 :doc:`js_host`。

方法 — host.net
---------------

IPv4 UDP。二进制载荷使用 **base64**。实现：``components/minihub_js/minihub_js_net.c``。

host.net.udp.open(opts?)
~~~~~~~~~~~~~~~~~~~~~~~~

``opts.broadcast`` （可选 bool）开启广播。返回小整数 socket 句柄。

host.net.udp.sendto(fd, { ip, port, data_b64 })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

host.net.udp.recvfrom(fd, { timeout_ms, max_len? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

返回 ``{ ip, port, data_b64 }``，超时为 ``null``。

host.net.udp.close(fd)
~~~~~~~~~~~~~~~~~~~~~~

方法 — host.crypto
------------------

摘要 / HMAC / AES。摘要返回 **小写 hex**；HMAC 与 AES 返回 **base64**。
输入可为字符串或 ``{ data }`` / ``{ data_b64 }``。

* ``host.crypto.md5(input)``
* ``host.crypto.sha1(input)``
* ``host.crypto.sha256(input)``
* ``host.crypto.hmacSha256({ key_b64, data_b64 })`` → base64
* ``host.crypto.aes128CbcEncrypt({ key_b64, iv_b64, data_b64 })`` → base64（PKCS7）
* ``host.crypto.aes128CbcDecrypt({ key_b64, iv_b64, data_b64 })`` → base64

示例：``examples/plugins/color_bulb_js/``。

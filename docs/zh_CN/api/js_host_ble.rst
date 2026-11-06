JS 插件 BLE Host API
====================

:link_to_translation:`en:[English]`

此命名空间实现在 ``components/minihub_js/minihub_js_ble.c``，底层为
``minihub_ble_gatt``。下表在构建时从 C 绑定读出。

.. minihub-JS-api:: ble

命名空间入口见 :doc:`js_host`。

方法 — host.ble
---------------

所有 BLE 厂商插件共用的**单实例、引用计数**被动广播扫描器。需启用
``CONFIG_MINIHUB_JS_BLE``（``menuconfig → JS plugins``）；未启用时下面的方法
抛出 ``host.ble disabled``，``setup(host)`` 可据此优雅降级。

实现：``components/minihub_js/minihub_js_ble.c``，底层为 ``minihub_ble_gatt``。

host.ble.ensureScan()
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

引用计数加一并返回新计数。首个调用者启动**连续扫描** （不做 UUID 过滤），
后续调用者只增加引用。请在 ``setup(host)`` 中调用；**不要** 每个模块各自启动
扫描，因为重启扫描器会清空共享的广播累积表，导致其它插件读不到数据。

host.ble.releaseScan()
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

引用计数减一并返回新计数，归零时停止扫描。模块卸载时运行时会自动释放引用。

host.ble.getAds({ company?, uuid16?, max? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

返回命中的广播数组（最多 ``max`` 条，默认 24）。``company`` 过滤 ``mfgCompany``；
``uuid16`` 同时匹配广播 UUID16 与 Service Data UUID16。过滤在 C 侧完成，插件
只需为自己真正会解析的对象付出构造成本。

数组元素::

  { addr: "aabbccddeeff", addrType: 0, rssi: -62, name: "sensor",
    uuid16: 0xfdcd,                        // 仅当广播中存在
    mfgCompany: 0x0499,                    // 仅当有 Manufacturer Data
    mfgData: [0x99, 0x04, 0x05, ...],     // 原始字节的小整数数组
    svcUuid16: 0xfdcd,                     // 仅当有 Service Data
    svcData: [0xcd, 0xfd, ...] }           // 原始字节的小整数数组

``mfgData`` / ``svcData`` **包含开头两个小端字节** （company id / UUID16），
与 C Host API 的交付格式完全一致。C 里的 ``d = mfg_data + 2`` 在 JS 中写作
``var d = ad.mfgData.slice(2)``。无需 base64 编解码。

请用 ``host.adapter.poll`` 拉取，而不是订阅事件：事件队列较浅，而 BLE 广播
是突发到达的。

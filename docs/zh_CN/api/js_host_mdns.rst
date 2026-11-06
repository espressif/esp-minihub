JS 插件 mDNS Host API
=====================

:link_to_translation:`en:[English]`

此命名空间实现在 ``components/minihub_js/minihub_js_mdns.c``，底层为
``espressif/mdns``。下表在构建时从 C 绑定读出。

.. minihub-JS-api:: mdns

命名空间入口见 :doc:`js_host`。

方法 — host.mdns
----------------

一次性局域网服务发现。需启用 ``CONFIG_MINIHUB_JS_MDNS``；未启用时方法抛出
``host.mdns disabled``。实现：``components/minihub_js/minihub_js_mdns.c``，
底层为 ``espressif/mdns``。

host.mdns.resolve(opts?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``opts``：``{ service: "_googlecast", proto: "_tcp", timeout_ms: 2000,
max_results: 8 }``。``timeout_ms`` 钳位到 100–10000，``max_results`` 钳位到
1–16。该调用会阻塞 JS worker 整个查询时长，请放在 ``host.timer`` 回调或显式
同步 RPC 中，**不要** 放进 5 s 轮询。

返回数组（无应答时为空）::

  { instance: "Living Room", service: "_googlecast", hostname: "chromecast.local",
    port: 8009, ip: "192.168.1.23", txt: { md: "Google Home" } }

mDNS 采用惰性且幂等的初始化，Matter/Thread 已占用的 responder 不受影响；
结果链表由绑定层负责释放。

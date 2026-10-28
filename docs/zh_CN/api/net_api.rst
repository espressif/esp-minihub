网络 C API
==========

:link_to_translation:`en:[English]`

概述
----

``minihub_net`` 封装 Host 上行（以太网 / Wi-Fi STA）初始化、IPv4 查询与 DNS 诊断。
启动顺序与 Kconfig 见 :doc:`../networking`。

模块应使用 ``minihub_net_*``，勿直接调用 ``esp_wifi_*``。
接口持久化配置见 ``minihub_net_cfg.h``；REST 入口为 :doc:`host_rest` 的
``POST /api/net/config``。

.. only:: not hide_l3

   ``esp_wifi_*`` 不在 L3 白名单内。

API 参考
--------

.. include-build-file:: inc/minihub_net.inc

.. include-build-file:: inc/minihub_net_cfg.inc

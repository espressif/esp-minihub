Plugin SDK — MiniHub
====================

:link_to_translation:`en:[English]`

概述
----

Store L3 插件使用的 MiniHub Host 导出：**点位**、**模块 ABI**、**配置 / hub 重载**、
**HTTP worker**、**网络探测**、**BLE GATT 客户端**。

仓库外插件通过 ``minihub_plugin_sdk/minihub.h`` （``#include "minihub_plugin_sdk.h"``）
获得相同声明。

Core 服务优先用 :doc:`module_abi` 中的 Host 虚表。点位类型与注册：:doc:`minihub_core`。
适配器虚表：:doc:`minihub_adapter`。网络：:doc:`net_api`。

HTTP worker
-----------

.. include-build-file:: inc/minihub_http_worker.inc

BLE GATT 客户端
---------------

GATT **客户端**，不是 BLE Mesh 射频。

.. include-build-file:: inc/minihub_ble_gatt.inc

相关
----

* 模块入口 / ``minihub_module_host_api_t``：:doc:`module_abi`
* 点位 / 命令 / 事件：:doc:`minihub_core`
* ``minihub_net_has_ip`` / ``minihub_net_dns_probe``：:doc:`net_api`
* 白名单与 ABI 校验：:doc:`plugin_host`

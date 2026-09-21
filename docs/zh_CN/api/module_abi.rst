模块 ABI（L3 原生插件）
========================

:link_to_translation:`en:[English]`

概述
----

Store **L3** 原生模块（``module.json`` + MHMD ``.so``）通过二进制 ABI 与 Host 交互。
模块导出 ``minihub_module_entry``，保存 ``minihub_module_host_api_t`` 虚表，并返回
``minihub_adapter_t``。L2 JS 见 :doc:`js_host`；REST 安装/RPC 见 :doc:`plugin_rest`。

运行时约束（摘要）：

* 与 Host 相同 IDF target（如 esp32s31）
* ``module.json`` 的 ``abi_version`` 与 ``core_api_min`` / ``core_api_max`` 须与 Host 匹配
* 未定义符号须为 Host 导出白名单的子集（见 :ref:`l3-host-export-allowlist` ）。
  **函数声明：** ``#include "minihub_plugin_sdk.h"`` （独立 SDK）— :doc:`plugin_host`
* 仅可在 ``minihub_module_host_api_t`` 结构体尾部追加字段；调用前检查 ``struct_size``
* 用函数指针 ``typedef`` 作为虚表槽位，布局与内联写指针相同。仅改名不要提高
  ``abi_version``。槽位顺序、签名或尾部发生变化时才提高。

API 参考
--------

.. include-build-file:: inc/minihub_module_abi.inc

.. _l3-host-export-allowlist:

符号白名单
----------

L3 ``.so`` 不得链接 Host 应用。每一个未定义 ELF 符号都必须出现在下方列表中
（与 ``docs/module_sdk/host_export_allowlist.json`` 相同）。Host 用
``esp_elf_register_symbol`` 解析。地址是固件本地的，不要写死。

**按前缀 / 范围包含**

* 编译器辅助 — ``__divdi3``、``__eqdf2``、``__extendsfdf2`` …
* libc — 堆、字符串、stdio、``time``、``open`` / ``read`` / ``write`` / ``close``
* cJSON — ``cJSON_*``
* ESP-IDF — ``esp_http_client_*``、``esp_log*``、``esp_timer_get_time``、
  ``esp_crt_bundle_attach``、``esp_err_to_name``、``esp_random`` / ``esp_fill_random``
* lwIP — ``lwip_socket``、``lwip_sendto`` / ``lwip_recvfrom``、``lwip_inet_*``
* 加密 — ``mbedtls_*``、``psa_*``
* NVS — ``nvs_*``
* MiniHub — ``minihub_point_*``、``minihub_config_*``、``minihub_http_*``、
  ``minihub_net_dns_probe`` / ``minihub_net_has_ip``、``minihub_hub_reload``、
  ``minihub_module_*``、``minihub_ble_gatt_*`` （GATT 客户端，不是 BLE Mesh 射频）
* FreeRTOS — ``vTaskDelay``、``xTaskCreatePinnedToCore``、队列 / 互斥量
* 堆查询 — ``heap_caps_get_free_size``

**排除：** Wi-Fi / Zigbee / BLE Mesh / Matter / OpenThread **射频** API；
``httpd`` URI 注册。

Host 重建后重新生成：

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

链接选项：``cmake/minihub_module_so.cmake``。

完整快照（以该 JSON 为运行时白名单真源）：

.. literalinclude:: ../../module_sdk/host_export_allowlist.json
   :language: json

C 函数原型：:doc:`plugin_host` （不要自行再声明这些符号）。
实现指南：:doc:`../plugins/develop_l3`。

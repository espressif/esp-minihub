模块 ABI（ELF 原生插件）
========================

:link_to_translation:`en:[English]`

概述
----

原生模块是被加载进一台正在运行的 Hub，所以在互相调用之前，两边必须先对一份二进制
契约达成一致。商店 **ELF** 模块（``module.json`` + MHMD ``.so``）就通过这份 ABI 与
Host 对接：模块导出 ``minihub_module_entry``，保存传入的 ``minihub_module_host_api_t``
虚表，并返回 ``minihub_adapter_t``。JS 见 :doc:`js_host`；REST 安装/RPC 见
:doc:`plugin_rest`。

契约成立的前提是下面每一条都满足：

* 与 Host 相同 IDF target（如 esp32s31）
* ``module.json`` 的 ``abi_version`` / ``core_api_min`` / ``core_api_max`` 要能
  **包住** Host 的版本 —— 而且三个都可省：省掉 ``abi_version`` 或
  ``core_api_min`` 就按 Host 自己的值取，``core_api_max`` 省掉记 0，表示不设上界。
  所以全都不写的包对任何 Host 都能加载；写上它们，才是让它拒绝不匹配的 Host
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

ELF ``.so`` 不得链接 Host 应用。每一个未定义 ELF 符号都必须出现在下方列表中
（与 ``docs/module_sdk/host_export_allowlist.json`` 相同）。Host 用
``esp_elf_register_symbol`` 解析。地址是固件本地的，不要写死。

**按前缀 / 范围包含**

* 编译器辅助 — ``__divdi3``、``__eqdf2``、``__extendsfdf2`` …
* libc — 堆（``malloc`` / ``calloc`` / ``realloc`` / ``free``）、字符串
  （``strcpy`` / ``strlen`` / ``strcmp`` / ``strcasecmp`` …）、``time``，以及
  ``snprintf`` / ``vsnprintf`` / ``sscanf``。**没有 ``printf`` 一族，没有
  ``gmtime`` / ``localtime``，也没有 POSIX 的 ``open`` / ``read`` / ``write``** ——
  文件读写只给 stdio 与目录调用：``fopen`` / ``fclose`` / ``fread`` / ``fwrite`` /
  ``fseek`` / ``ftell`` / ``rewind`` / ``close`` / ``mkdir`` / ``rmdir`` /
  ``unlink`` / ``stat``
* cJSON — ``cJSON_*``
* ESP-IDF — ``esp_http_client_*``、``esp_timer_get_time``、
  ``esp_crt_bundle_attach``、``esp_err_to_name``、``esp_random`` / ``esp_fill_random``；
  打日志只有 ``esp_log`` 与 ``esp_log_timestamp``：``ESP_LOGx`` 宏展开成
  ``esp_log_write``，而它 **不在** 白名单，所以要用 SDK 头文件（它把 ``ESP_LOGI``
  重新映射到 ``esp_log``），不要直接引 IDF 的 ``esp_log.h``
* lwIP — ``lwip_socket``、``lwip_sendto`` / ``lwip_recvfrom``、``lwip_setsockopt``、
  ``lwip_inet_pton`` / ``lwip_inet_ntop``
* 加密 — ``mbedtls_*`` （base64、摘要、HMAC）、``psa_*``
* NVS — ``nvs_*``
* MiniHub — 点位注册表与状态按名字列出：``minihub_point_register``、
  ``minihub_point_set``、``minihub_point_set_online``、``minihub_point_get``、
  ``minihub_point_get_snapshot``、``minihub_point_merge_params``、
  ``minihub_point_merge_props``、``minihub_point_list_by_adapter``、
  ``minihub_point_list_free``、``minihub_point_unregister_by_adapter``。
  ``minihub_point_unregister`` **不** 导出，``minihub_command_register_adapter``
  与 ``minihub_adapter_register`` 也不导出 —— 那几个走虚表。另有
  ``minihub_command_invoke``、``minihub_config_*``、``minihub_http_*``、
  ``minihub_net_dns_probe`` / ``minihub_net_has_ip``、``minihub_hub_reload``、
  ``minihub_module_*``，以及 BLE GATT 客户端那一组：``minihub_ble_gatt_init`` /
  ``ready`` / ``scan_start`` / ``scan_stop`` / ``scan_get`` / ``connect`` /
  ``disconnect`` / ``is_connected`` / ``discover_svc`` / ``find_char`` /
  ``subscribe`` / ``wait_notify`` / ``write`` / ``conn_handle`` / ``deinit``
  （GATT 客户端，不是 BLE Mesh 射频）。这份列表由
  ``tools/gen_host_export_symbols.py`` 从 Host 运行时注册的那张
  ``s_minihub_host_elfsyms[]`` 表推导，所以 Host 常驻导出的符号一定在里面 ——
  少了某个符号是生成器的问题，该报修，不要在插件里绕。
* FreeRTOS — ``vTaskDelay``、``vTaskDelete``、``xTaskCreatePinnedToCore``、
  ``xQueueCreateMutex``、``xQueueGenericSend``、``xQueueSemaphoreTake``。
  ``xTaskCreate``、``xQueueSend``、``xSemaphoreTake`` 都不导出 —— 建任务要用
  绑核那个名字，队列与信号量取通用名那两个
* 堆查询 — ``heap_caps_get_free_size``

**排除：** Wi-Fi / Zigbee / BLE Mesh / Matter / OpenThread **射频** API；
``httpd`` URI 注册。

Host 重建后重新生成：

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

链接选项：``cmake/minihub_module_so.cmake``。

完整快照（以该 JSON 为运行时白名单真源）：

.. literalinclude:: ../../module_sdk/host_export_allowlist.json
   :language: JSON

C 函数原型：:doc:`plugin_host` （不要自行再声明这些符号）。
实现指南：:doc:`../plugins/develop_elf`。

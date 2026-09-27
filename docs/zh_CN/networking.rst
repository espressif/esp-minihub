网络
========

:link_to_translation:`en:[English]`

MiniHub 在 ``components/minihub_net`` 中基于 ESP-IDF 的 ESP-NETIF、Wi-Fi 驱动
及（带 EMAC 的 SoC 上）以太网实现 IP 上行。

上游文档：

* `ESP-IDF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_
* `ESP-NETIF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif.html>`_
* `Wi-Fi 驱动 <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi-driver/index.html>`_

Matter 控制器侧网络见 :doc:`matter`。C API 见 :doc:`api/net_api`。

Kconfig
-------

* ``CONFIG_MINIHUB_NET_WIFI_STA`` — 创建 ``WIFI_STA_DEF``，凭据连接
* ``CONFIG_MINIHUB_NET_ETH`` — 原生 EMAC + PHY（S31 Function CoreBoard：YT8531）
* ``CONFIG_MINIHUB_NET_WIFI_SOFTAP`` — SoftAP/STA 由 ``minihub_net`` 外管理
* ``CONFIG_MINIHUB_WIFI_PROVISIONING`` — 无 NVS 凭据时 SoftAP 和/或 BLE 配网

片段：``config/sdkconfig/feature/wifi_sta.defaults``、``eth.defaults``。

启动顺序
--------

1. ``minihub_net_init()`` — netif/事件循环；启动以太网；Wi-Fi **仅注册处理** （日志含 ``wifi STA deferred``）。
2. ``minihub_adapter_start_all()`` — 射频互斥后按 Wi-Fi → Zigbee 或 Thread →
   BLE Mesh → 其它适配器 start。``wifi`` 适配器调用 ``minihub_net_start_wifi()``
   （``esp_wifi_start`` + 连接，或进入配网）。

默认路由与 STA 连接
-------------------

**以太网已有 IPv4 时，仍会做 Wi-Fi 关联。**

* **有 STA 凭据** （Kconfig ``CONFIG_MINIHUB_WIFI_SSID`` 或 NVS）— ``minihub_net_start_wifi()`` 会 ``esp_wifi_start`` 并 ``connect`` ；以太网已有 IPv4 也不会跳过。无凭据则走配网（或失败）。配网进行中不会 ``connect``
* **默认路由** — ``minihub_net_refresh_default_netif()``：以太网有 IPv4 则走 Eth，
  否则 Wi-Fi 有 IPv4 则走 Wi-Fi，否则以太网（若已编译）或 Wi-Fi
* **以太网链路断开** — 日志 ``Ethernet link down`` 后再选一次默认路由；仍是
  「eth 还有 IPv4 就走 eth」，Wi-Fi 有 IP 才会成为默认口

凭据与配网
----------

Wi-Fi SSID/PSK 保存在 NVS，由 ``minihub_provision_load_credentials()`` 加载。

**无凭据** 且打开 ``CONFIG_MINIHUB_WIFI_PROVISIONING`` 时：

* ``minihub_net_start_wifi()`` 之后调用 ``minihub_provision_start()``
* 传输方式由 Kconfig 决定：SoftAP （``CONFIG_MINIHUB_WIFI_PROV_TRANSPORT_SOFTAP``）
  和/或 BLE （``CONFIG_MINIHUB_WIFI_PROV_TRANSPORT_BLE``）

REST：网络配置
--------------

``POST /api/net/config`` 按接口设置 DHCP 或静态 IP。

**请求体**

.. code-block:: json

   {
     "iface": "eth",
     "dhcp": false,
     "ip": "192.168.1.50",
     "netmask": "255.255.255.0",
     "gw": "192.168.1.1",
     "dns": ["8.8.8.8", "1.1.1.1"]
   }

亦可用 ``dns_main`` / ``dns_backup`` 代替 ``dns`` 数组。

* ``iface`` — ``eth`` 或 ``wifi`` （必填）
* ``dhcp`` — 布尔 （必填）
* ``dhcp`` 为 false 时需填写 ``ip``、``netmask``、``gw``

**响应**

.. code-block:: json

   { "result": "ok" }

失败：``{ "result": "fail", "error": "ESP_ERR_..." }``。

示例（curl）：

.. code-block:: bash

   curl -s -X POST http://HUB/api/net/config \
     -H 'Content-Type: application/json' \
     -d '{"iface":"wifi","dhcp":true}'

预期日志
--------

* ``Ethernet started`` — EMAC 已启（链路可能仍未 up）
* ``Ethernet link up, MAC …`` — 物理链路已接通
* ``eth got IP`` / ``wifi got IP`` — 该接口已获得 IPv4
* ``MiniHub ready (net=… prov=… web_ui=… lcd_ui=… eth=… wifi=…)`` — 功能已准备，不代表已有 IP

排障
----

**默认路由走以太网但 Wi-Fi 无 IP**

确认 NVS 有 STA 凭据、``adapters.wifi.enabled`` 为 true，且 ``wifi`` 适配器
已调用 ``minihub_net_start_wifi()``。默认路由选择不会跳过 STA 连接。

**STA 始终不连接**

* 确认配网已完成（Web UI 或 SoftAP/BLE）
* 查找 ``wifi got IP``；若无则检查 NVS 中 SSID/PSK
* 确认 Matter/CHIP 未占用第二套 STA（见 :doc:`matter`）

**双接口均 up — 走哪条路？**

以太网有 IPv4 则走以太网，否则走 Wi-Fi。拔网线后 Wi-Fi 有 IP 即可回退
（``minihub_net_refresh_default_netif``）。

**REST net config 返回 fail**

``dhcp: false`` 时静态字段须合法。请求体上限约 2 KB。

C API
-----

声明于 ``components/minihub_net/include/minihub_net.h``：

* ``minihub_net_init`` / ``minihub_net_start_wifi``
* ``minihub_net_has_ip`` / ``minihub_net_get_ip`` / ``minihub_net_get_netif``
* ``minihub_net_refresh_default_netif`` — 按 IPv4 上行选择默认 netif
* ``minihub_net_dns_probe`` (导出给 Store 模块)

接口 DHCP / 静态 IP 在 ``minihub_net_cfg.h``：

* ``minihub_net_set_iface_config`` — ``POST /api/net/config`` 所用
* ``minihub_net_get_iface_status`` · ``minihub_net_apply_saved_configs``

结构体与返回码见 :doc:`api/net_api`。

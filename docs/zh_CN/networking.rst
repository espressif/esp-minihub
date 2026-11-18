网络
========

:link_to_translation:`en:[English]`

Hub 多数功能都要先有 IP，所以网络很早起来。``components/minihub_net`` 基于
ESP-IDF 的 ESP-NETIF、Wi-Fi 驱动，以及带 EMAC 的 SoC 上的以太网，搭出 IP 上行。
三者都是 IDF 层，上游文档如下：

* `ESP-IDF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_
* `ESP-NETIF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif.html>`_
* `Wi-Fi 驱动 <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi-driver/index.html>`_

Matter 控制器侧网络见 :doc:`matter`。C API 见 :doc:`api/net_api`。

Kconfig
-------

* ``CONFIG_MINIHUB_NET_WIFI_STA`` — 创建 ``WIFI_STA_DEF``，凭据连接
* ``CONFIG_MINIHUB_NET_ETH`` — 原生 EMAC + PHY（S31 Function CoreBoard：YT8531）
* ``CONFIG_MINIHUB_NET_WIFI_SOFTAP`` — SoftAP/STA 由 ``minihub_net`` 外管理
* ``CONFIG_MINIHUB_WIFI_PROVISIONING`` — 无 NVS 凭据时走 SoftAP 配网
* ``CONFIG_MINIHUB_MQTT_BROKER`` — Hub 内置 MQTT broker；是否监听由运行时配置决定

片段：``configs/sdkconfig/feature/wifi_sta.defaults``、``eth.defaults``、
``mqtt_broker.defaults``。

启动顺序
--------

.. image:: ../_static/net_boot/boot_flow.svg
   :align: center
   :alt: MiniHub 网络启动：先起以太网 → 轮到 Wi-Fi 适配器 → 连 STA → 选默认路由 → 控制台上线

`交互版 <_static/net_boot/index.html>`_ —— 按开机顺序走五站，青色回程线是没有凭据时走的那条配网路。

1. ``minihub_net_init()`` — netif/事件循环；启动以太网；Wi-Fi **仅注册处理** （日志含 ``wifi STA deferred``）。
2. ``minihub_adapter_start_all()`` — 射频互斥后按 Wi-Fi → Zigbee 或 Thread →
   BLE Mesh → 其它适配器 start。``wifi`` 适配器调用 ``minihub_net_start_wifi()``
   （``esp_wifi_start`` + 连接，或进入配网）。

默认路由与 STA 连接
-------------------

**以太网已有 IPv4 时，仍会做 Wi-Fi 关联。**

* **有 STA 凭据** （Kconfig ``CONFIG_MINIHUB_WIFI_SSID`` 或 NVS）— ``minihub_net_start_wifi()`` 会 ``esp_wifi_start`` 并 ``connect``；以太网已有 IPv4 也不会跳过。无凭据则走配网（或失败）。配网进行中不会 ``connect``
* **默认路由** — ``minihub_net_refresh_default_netif()``：以太网有 IPv4 则走 Eth，
  否则 Wi-Fi 有 IPv4 则走 Wi-Fi，否则以太网（若已编译）或 Wi-Fi
* **以太网链路断开** — 日志 ``Ethernet link down`` 后再选一次默认路由；仍是
  「eth 还有 IPv4 就走 eth」，Wi-Fi 有 IP 才会成为默认口

凭据与配网
----------

Wi-Fi SSID/PSK 保存在 NVS （命名空间 ``minihub_wifi``），由
``minihub_provision_load_credentials()`` 加载。NVS 优先于
``CONFIG_MINIHUB_WIFI_SSID`` / ``CONFIG_MINIHUB_WIFI_PASS``，而字面占位值
``YOUR_SSID`` 按“未设置”处理。

**无凭据** 且打开 ``CONFIG_MINIHUB_WIFI_PROVISIONING`` 时，Hub 开一个明文开放热点
—— SSID ``PROV_MiniHub``、无密码、信道 1、最多 4 个客户端 —— 然后从这个热点上进
控制台，用 ``POST /api/net/wifi`` 配 Wi-Fi。这里刻意 **不** 调
``minihub_provision_start()``：ESP-IDF 的配网管理器自己要在 80 端口起页面，而 80
是控制台的端口。Wi-Fi 配网没有蓝牙这条路 —— BLE 传输已移除，配网不需要
``CONFIG_BT_ENABLED``，也不需要额外的配对工具，浏览器就够了。

管理器入口留给把控制台挪到别的端口的产品。``CONFIG_MINIHUB_WIFI_PROVISIONING=n``
连那个开放热点也不开，此时无凭据的设备只留一行
``no Wi-Fi credentials and provisioning disabled`` 日志。

MQTT broker
-----------

``components/minihub_mqtt_broker`` 在 Hub 上跑一个
`mosquitto <https://components.espressif.com/components/espressif/mosquitto>`_
broker，站点里的 MQTT 设备因此连 Hub 本身，不再需要一台常开的机器。它只做传输：
不注册任何点位。打开它的同时，Hub 也会在 ``127.0.0.1`` 上起一个自己的 MQTT 客户端
—— 与 broker 同属 ``minihub_mqtt_broker`` （``mqtt_adapter.c``），登记在同一个
``mqtt_broker`` 名字下 —— 因为 JS 插件用的 ``host.mqtt`` 就挂在这个客户端上。
没有连外部 broker 的模式：broker 不启动，客户端也就不会被创建。

只要 ``CONFIG_MINIHUB_MQTT_BROKER`` 打开（默认打开），固件里就带着 broker；是否监听
由运行时的 ``adapters.mqtt_broker`` 决定（:ref:`adapters-JSON`），出厂种子是关的：

.. code-block:: JSON

   "mqtt_broker": {
     "enabled": false,
     "port": 1883,
     "allow_anonymous": true,
     "users": [],
     "connect_rate_limit": 16
   }

* ``enabled`` — 出厂种子里是 ``false``：不建任务，不开端口，也不创建环回客户端
* ``port`` — TCP 监听端口，1–65535，默认 1883
* ``allow_anonymous`` — 默认 ``true``，不带用户名的客户端直接放行。带了用户名的
  客户端一律查 ``users``，查不到就拒绝。设成 ``false`` 之后，Hub 自己的客户端用
  ``users[0]`` 登录，所以 ``users`` 为空就等于 ``host.mqtt`` 连不上，并会记一条告警
* ``users`` — 最多 4 条 ``{"name": …, "pass": …}``，每个字段最长 31 字节，超出的
  丢弃并告警
* ``connect_rate_limit`` — 每 10 秒窗口内接受的 CONNECT 报文数，默认 16。同时在线的
  客户端数没有上限

监听绑 ``0.0.0.0``，所以以太网、Wi-Fi STA 地址、配网热点（``192.168.4.1``）上都会
应答。默认姿态是匿名放行且没有按 topic 的 **访问控制列表（ACL）**，也就是说任何能连到 Hub 的设备都能
发布和订阅全部 topic。出货前把 ``allow_anonymous`` 设成 ``false`` 并列出用户；网络
前提见 :doc:`security`。

只有 ``mqtt_broker`` 这棵子树真的变了，reload 才会重启 broker，所以改别的适配器不会
让 MQTT 客户端掉线；重启则会。retained 消息和离线会话只存在 RAM 里，重启或 broker
重启之后就没了。

这一版没有：TLS、WebSocket、桥接、持久化、按 topic 的 ACL，也只有一个 listener。
``CONFIG_MOSQ_ENABLE_SYS`` 是调试开关（默认关），打开后任何 MQTT 客户端都能读到
broker 自己的 ``$SYS`` 计数。

REST：网络配置
--------------

``POST /api/net/config`` 按接口设置 DHCP 或静态 IP。

**请求体**

.. code-block:: JSON

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

.. code-block:: JSON

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
* ``listening on 0.0.0.0:1883, anonymous allowed, 0 user(s)`` — MQTT broker 任务已起（tag ``mqtt_broker``）

排障
----

**默认路由走以太网但 Wi-Fi 无 IP**

确认 NVS 有 STA 凭据、``adapters.wifi.enabled`` 为 true，且 ``wifi`` 适配器
已调用 ``minihub_net_start_wifi()``。默认路由选择不会跳过 STA 连接。

**STA 始终不连接**

* 确认配网已完成（Web UI 或 SoftAP 流程）
* 查找 ``wifi got IP``；若无则检查 NVS 中 SSID/PSK
* 确认 Matter/CHIP 未占用第二套 STA（见 :doc:`matter`）

**双接口均 up — 走哪条路？**

以太网有 IPv4 则走以太网，否则走 Wi-Fi。拔网线后 Wi-Fi 有 IP 即可回退
（``minihub_net_refresh_default_netif``）。

**REST net config 返回 fail**

``dhcp: false`` 时静态字段须合法。请求体上限约 2 KB。

**MQTT 设备连不上 broker**

* ``adapters.mqtt_broker.enabled`` 须为 ``true``，然后在控制台重载，或调
  ``POST /api/hub/reload``
* 出现 ``listening on …`` 说明端口已绑上；出现 ``mosq_broker_run returned …`` 说明
  broker 退出了，每 5 秒重试一次 —— 此时查一下端口是否被别的服务占着
* 客户端带了用户名就一定查 ``users``，查不到即拒，``allow_anonymous`` 为 ``true``
  也一样
* 反复重连的设备可能耗尽 ``connect_rate_limit``，在那 10 秒窗口剩下的时间里都会被拒

C API
-----

声明于 ``components/minihub_net/include/minihub_net.h``：

* ``minihub_net_init`` / ``minihub_net_start_wifi``
* ``minihub_net_has_ip`` / ``minihub_net_get_ip`` / ``minihub_net_get_netif``
* ``minihub_net_refresh_default_netif`` — 按 IPv4 上行选择默认 netif
* ``minihub_net_dns_probe`` (导出给商店模块)

接口 DHCP / 静态 IP 在 ``minihub_net_cfg.h``：

* ``minihub_net_set_iface_config`` — ``POST /api/net/config`` 所用
* ``minihub_net_get_iface_status`` · ``minihub_net_apply_saved_configs``

结构体与返回码见 :doc:`api/net_api`。

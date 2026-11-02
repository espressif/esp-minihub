Host REST API
=============

:link_to_translation:`en:[English]`

概述
----

Host Web 栈对外提供的 HTTP JSON API，供 OEM Host、局域网集成、自动化与运维使用。
默认端口 **80**。WebSocket GET /ws/events 与规则引擎共用同一事件总线。

实现位于 components/minihub_web/minihub_web.c。将示例中的 HUB 替换为设备
IPv4（日志 eth got IP / wifi got IP）。

商店插件安装与 RPC 见 :doc:`plugin_rest`；C 侧命令路由见 :doc:`minihub_core`；
状态码与错误正文见 :doc:`errors`。

通用约定
--------

* 请求/响应体一律是 JSON，唯独 ``POST /api/ota/upload`` 是带 ``Content-Length``
  的原始 ``.bin`` 请求体（不是 multipart，也不检查 ``Content-Type``）。
* 打开 ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认）时，使用 ``mh_sess`` Cookie 或
  ``Authorization: Bearer``，否则 **401**。公开接口：
  ``GET /api/auth/status``、 ``POST /api/auth/setup``、 ``POST /api/auth/login``。
  可选 token：``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN``，**缺省为空**；非空时以它为准，
  只有编进来的串为空才会去读 NVS 键 ``api_token`` （命名空间 ``minihub`` ）。所以出厂
  状态下，脚本要么用每台设备自己的 NVS token ，要么用控制台会话，而不是共用一个字符串。
* 许多写操作成功时返回 ``{"result":"ok"}``；失败时为 HTTP 状态码 + 纯文本或 JSON。


接口总览
--------

下面这张表在构建时直接从 ``components/minihub_web/minihub_web.c`` 的
``minihub_web_attach()`` 读出来：每个已注册路由的 URI、方法、处理函数是否校验登录，
以及它背后的 Kconfig 开关。页面其余部分只细写插件与集成会直接调用的那些。

.. minihub-routes:: pages

.. minihub-routes:: auth

.. minihub-routes:: devices

.. minihub-routes:: config

.. minihub-routes:: store

.. minihub-routes:: update

.. minihub-routes:: automation

.. minihub-routes:: modules

.. minihub-routes:: radios

.. minihub-routes:: net

.. minihub-routes:: dev

``GET /ws/events`` 是 WebSocket，不是 ``httpd_uri_t`` ，所以不在这张表里，见本页末尾的专节。

POST /api/command
-----------------

.. container:: mh-api-block mh-api-command

   ``POST /api/command``

将命令按 point_id 路由到所属适配器的 handle_command，等价 MCP command.invoke。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``point_id`` (string)
       目标点位 id，最长 64 字符；须已注册。

   ``cmd`` (string)
       命令名（如 turn_on），最长 32 字符；由适配器解释。

   ``params`` (object, optional)
       传给适配器的 JSON。目录设备请设 ``parameter`` 为参数名（如
       ``brightness``），适配器据此更新该设备点上的 ``props.params``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — 空 body、JSON 无效、缺少 point_id 或 cmd
   * **HTTP 404** — 目标点位未注册
   * **HTTP 500** — 适配器拒绝该命令
   * **HTTP 401** — 启用 auth 时未鉴权

.. container:: mh-api-block mh-api-attention

   **Attention**

   请求体只读入 256 字节缓冲，请保持请求体短小；更大的负载改用
   ``POST /api/devices/control`` （1 KiB）或模块 RPC。params 形状因适配器而异。
   点位变更经 WebSocket point.updated 推送（见 GET /ws/events）。

GET /api/points
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points``

返回当前点位注册表快照。

.. container:: mh-api-block mh-api-parameters

   **Parameters（查询串）**

   ``point_id`` (string, optional)
       只返回该点位对象，而不是整个数组；未注册时 **404**。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 不带 ``point_id`` 时返回 JSON 数组，元素含 ``point_id``、
     ``type``、``value``、``adapter``、``online``、``updated_ms``，以及
     ``props`` （适配器填了则含 ``params``）；带 ``point_id`` 时返回同样字段的
     单个对象

.. container:: mh-api-block mh-api-attention

   **Attention**

   字段上限见 :doc:`minihub_core` 中的容量宏。一台设备一条 point；实时参数在
   ``props.params``。类型文案/控件：``GET /api/device_widgets``。

GET /api/device_widgets
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/device_widgets``

类型目录，供概览页和规则编辑器。不是实例状态。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"devices":[…],"parameters":{…}}``。每条含 ``device`` （类型 id，如
     ``light``）、``image`` （类图标 URI 或空）、``params[]``：``parameter``、
     ``label``、``widget`` （``toggle`` / ``slider`` / ``dropdown`` /
     ``text`` / ``button`` / ``hidden``）、``image`` （恒为空，控件走 CSS）、
     ``style`` （原样带上目录行：``widget``、``labels``、``min`` / ``max`` /
     ``step`` / ``unit``、``access``、``options``、``css``、``ui_element``、
     ``requires``），以及可选 ``point_id`` （已登记的点里，该类型且带这个参数的
     第一条）。
   * 顶层 ``parameters`` 以参数名为键，覆盖上传的 ``model/parameters.csv`` 里的
     每一行，包括没有任何类型在用的参数；控制台就是靠它给「目录里查不到的类型的
     参数」取名。没上传过模型 CSV 的原机，这里是空对象。

.. container:: mh-api-block mh-api-attention

   **Attention**

   目录与协议无关。传输 ``point_id`` 来自登记（``协议.设备``）。隐藏参数只看实例
   ``props.params.名称.visible``；目录里虽然有 ``visible`` 这一列，但它的值不被读取。
   表结构见 ``components/minihub_device_widget/README_CN.md``。

POST /api/devices/control
-------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/devices/control``

概览页写入：解析已登记点，再走与 ``POST /api/command`` 相同的命令路径。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON body）**

   ``device`` (string)
       数据模型类型（``light``、``switch`` 等）。

   ``parameter`` (string)
       目录参数名（``power``、``brightness`` 等）。

   ``value`` (string 或 JSON)
       新值。

   ``point_id`` (string, optional)
       具体实例（如 ``sim.kitchen_lamp``）。只有该点存在、其 ``type`` 与
       ``device`` 一致，**且** 它的 ``props.params`` 里确实带 ``parameter`` 时，
       Host 才用它；否则回退到 ``(类型, 参数)`` 的首次匹配。

   ``seq`` (integer, optional)
       上报序号，缺省 0；同时写进 payload 与 ``params.seq``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok","path":…,"parameter":…,"payload":…}``，
     ``path`` 是实际寻到的点位（省略 ``point_id`` 时靠它确认打到了哪台），
     ``payload`` 是适配器收到的上报 JSON
   * **HTTP 400** — 空 / 截断 body、超过 1 KiB、JSON 无效、缺字段，或序列化后的
     ``value`` 达到 256 字节
   * **HTTP 404** — 没有任何已登记点带这组 ``(类型, 参数)``，或适配器返回
     ``ESP_ERR_NOT_FOUND``
   * **HTTP 500** — 内存不足、上报 payload 超过 384 字节，或其他适配器错误
   * **HTTP 401** — 开启登录鉴权却未鉴权

.. container:: mh-api-block mh-api-attention

   **Attention**

   寻址看的是在线点位，不看目录：只要某条已登记点的 ``props.params`` 里真有这个
   参数，就允许下发，哪怕类型目录从来没列过它（米家这类插件就是如此）。参数名被
   复制进 64 字节的缓冲，过长会被截断。

   发给适配器的命令名，在 ``value`` 是 JSON 布尔时是 ``toggle``，其余情况是
   ``set`` —— 也就是说 ``{"value":true}`` 与 ``{"value":false}`` 都走
   ``toggle``；需要区分开关就传字符串（``"on"`` / ``"off"``）。Host 会把
   ``value`` 做 JSON 序列化（字符串带引号）：数字用 JSON 数字，非数字下拉项用字符串。

   概览页 UI 总会带上卡片的 ``point_id``，同类型多台可各自控制；省略时按最先匹配的
   一条走。自动化也可用带明确 ``point_id`` 的 ``POST /api/command``。

POST /api/devices/ota
---------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/devices/ota``

触发已登记点位的 **终端设备 OTA** （不是 Hub 固件 ``/api/ota``）。按该点
``adapter`` 调用命令 ``ota``。插件在 ``props.ota`` 声明是否可升级（见
:doc:`../data_model`）。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``point_id`` (string, required)
       目标设备点位。

   ``path`` / ``file_path`` / ``url`` (string, optional)
       镜像路径或 URL；三者只取 **第一个** 出现的。都没写时，优先把
       ``props.ota.file`` 作为命令的 ``path``，否则用 ``props.ota.url``。
       请求体上限 512 字节。

能不能升，取决于射频本身，不是设备答不答应：

* **Matter** —— 走 CSA OTA Provider / Requestor 与 BDX，但要求构建同时打开
  ``CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED`` 与
  ``CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER`` （Matter 特性配置里是开的）。
  传本地文件路径时会解析 Matter OTA 镜像头，没有头就拒；传 ``http(s)`` URL 时
  直接交给请求端，不做解析。
* **Zigbee** 与 **BLE Mesh** —— 空中路径目前没实现，因此一律返回
  ``ESP_ERR_NOT_SUPPORTED``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``，只表示已把 offer 交给适配器，不代表设备
     刷写成功
   * **HTTP 400** — 缺 ``point_id``，或适配器返回 ``ESP_ERR_NOT_SUPPORTED``
   * **HTTP 404** — 未知点位
   * **HTTP 500** — 其他适配器错误。注意：请求里没有任何镜像地址时，Host **不会**
     提前拒绝，命令照样下发（不带镜像），适配器的 ``ESP_ERR_INVALID_ARG`` 于是变成
     500；Matter 在控制器未就绪时返回的 ``ESP_ERR_INVALID_STATE`` 同样记 500。

.. container:: mh-api-block mh-api-attention

   **Attention**

   ``props.ota`` 里 Host 代码真正读的只有 ``file``、``url``、``version``；
   ``available`` 与 ``status`` 是插件与控制台之间的约定。控制台按 ``available``
   显示可升级入口，但只写了 ``url`` 的插件依然可以通过 REST 升级。

GET /api/points/history
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points/history``

返回单个点位的近期 value 变更。Core 用一条 **全局** 160 条的环形缓冲记录所有
点位的 value 变化（value 字符串没变则不记），读取时按 ``point_id`` 过滤。

.. container:: mh-api-block mh-api-parameters

   **Parameters（查询串）**

   ``point_id`` (string, required)
       目标点位。

   ``limit`` (integer, optional)
       条数上限：默认 48；接受 1–128，但 Core 单次最多回放 64 条。全局环只有
       160 条且被所有点位共用，所以变更频繁的点会挤掉其它点的历史（MCP
       ``points.history`` 上限 50）。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"items":[…]}``，每条含 ``ts_ms``、``old``、``new``、
     ``source`` （本地条目恒为 ``local``），新的在前；``old`` / ``new`` 截断到
     31 个字符
   * **HTTP 400** — 缺少 ``point_id``
   * **HTTP 404** — 点位未注册

POST /api/points/remove
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/points/remove``

从注册表移除点位；若配置中存在则同步更新持久化。

.. warning::

   这是唯一一次「重启也回不来」的点位操作：持久化的注册信息一起删掉了，只有它的适配器
   或包重新登记这个 ``point_id`` 时，点位才会出现。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``point_id`` (string, required)
       要移除的点位 id。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — 点位不存在

POST /api/points/rename
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/points/rename``

设置已登记点位的显示名并持久化。名字写入该点的 ``props.name``，也写入设备名
存储，下次启动仍生效。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体，最大 512 字节）**

   ``point_id`` (string, required)
       已登记点位。

   ``name`` (string, required)
       显示名；超过 48 字符会被截断。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — 缺字段、名字为空或无效
   * **HTTP 404** — 点位未注册

GET /api/adapters
-----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/adapters``

给 Web UI 用的适配器 **概览** （芯片 / 启用标志）。 **不是** 完整
``adapters.json`` —— 全文见 ``GET /api/config/adapters``。

实现：``handle_api_adapters`` → ``minihub_module_overview_adapters_json``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 以适配器名为键的 JSON 对象。每项含 ``enabled``、``auth``；
     已知时还有 ``module_id``、``module_name``、``module_version``、
     ``adapter_version``。

GET /api/config/adapters
------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/config/adapters``

返回完整运行时 adapters.json（LittleFS /data/config/adapters.json）。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 对象（字段见 :doc:`../data_model`）

PUT /api/config/adapters
------------------------

.. container:: mh-api-block mh-api-command

   ``PUT /api/config/adapters``

整体替换并持久化 adapters.json。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   完整 adapters.json 根对象；字段见 :doc:`../data_model`。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 通常为 ``{"result":"ok"}``

.. container:: mh-api-block mh-api-attention

   **Attention**

   勿在通用 Host 出厂配置中写入厂商云 token；凭据由插件在用户登录后写入 NVS。PUT 覆盖整
   文件，集成侧应先 GET 再合并修改。

POST /api/hub/reload
--------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/hub/reload``

软重载 Hub：重读 JSON、重载 L1 包与商店模块、重启适配器与 JS，不整机关机。
详见 :doc:`../architecture`。

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   无，或空 JSON body。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``

.. container:: mh-api-block mh-api-attention

   **Attention**

   MCP hub.reload 限速每 5 秒一次；REST 无相同限速，但重载期间 MCP 可能返回
   HUB_NOT_READY。Matter + Thread 构建重载耗时更长。

GET /api/system
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/system``

返回 Hub 运行摘要，供 Web UI 与运维脚本使用。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON，含 ``name``、``version``、``idf_version``、``commit``、
     ``uptime_s``、``chip`` （``chip_cores`` / ``chip_revision``、``flash_size``）、
     ``ram`` （``internal_free`` / ``internal_total`` / ``psram_free`` /
     ``psram_total``）、``storage`` 与 ``modstore`` 占用、``local_ip``、
     ``web_base``、``net`` （``eth`` / ``wifi``，后者带 ``ssid`` 与
     ``has_password``），以及 ``timezone`` （``id``、``offset_min``、``zones``）。

   这里没有墙钟字段：``uptime_s`` 从启动开始计数，所以它告诉你 Hub 跑了多久，不告诉你
   设备认为现在是几点 （ :doc:`../time` ）。

POST /api/system/reboot · POST /api/system/reset
------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/system/reboot · POST /api/system/reset``

reboot 约半秒后重启；reset 安排一次恢复出厂。两者都要求 JSON 请求体
``{"confirm": true}``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}`` （reboot 先应答再重启）
   * **HTTP 400** — 无请求体，或 ``confirm`` 不为 ``true``

.. warning::

   ``reset`` 就是恢复出厂：规则 / 场景 / 适配器、所有由商店安装的模块与包，以及整个 NVS
   （含 Wi-Fi 凭据、商店 URL 与控制台密码）都会被清掉；保留的是出厂预装与 Web 控制台。完整
   范围，以及为什么复位后的设备在设好密码之前是不设防的，见 :doc:`../production` 。

GET / POST /api/system/timezone
-------------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/system/timezone``

GET 返回 ``id``、``offset_min`` 和可选的 ``zones`` 列表。POST 请求体为
``{"id":"UTC+8"}`` 或 ``{"id":"UTC+5:30"}``；偏移范围是 UTC−12 到 UTC+14，
整点、半小时、一刻钟都收（分钟只能是 00 / 15 / 30 / 45），零写 ``UTC``。选择写入
NVS 并立即生效。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 与 GET 相同的状态 JSON
   * **HTTP 400** — 未知 ``id``

GET /api/location
-----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/location``

按公网 IP 估算位置，结果来自缓存。``?refresh=1`` 启动后台刷新，本次响应仍是缓存
值。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``ok`` （首次查询成功前为 false）；已知时还有 ``lat``、
     ``lon``、``city``、``region``、``country``、``address``、``map_provider``
     （``amap`` 或 ``osm``）、``public_ip``；Hub 有地址时总会带 ``local_ip``。

POST /api/net/config
--------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/net/config``

设置以太网或 Wi-Fi 的 DHCP/静态 IP。详见 :doc:`../networking`。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``iface`` — eth 或 wifi

   ``dhcp`` — 布尔；true 为 DHCP

   ``ip``、``netmask``、``gw`` — 静态地址（dhcp: false 时）

   ``dns`` 或 ``dns_main`` / ``dns_backup`` — DNS 服务器

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``；写入 NVS 并尝试立即生效

POST /api/net/wifi
------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/net/wifi``

从控制台设置 Wi-Fi STA 凭据。Hub 保存后，STA 已启用时立即连接；处于 SoftAP
配网或 Wi-Fi 尚未启动时，凭据在下次启动生效。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体，最大 512 字节）**

   ``ssid`` (string, required)
       1–32 字符。

   ``password`` (string, optional)
       最长 64 字符。为空时沿用已存密码，除非 ``open`` 为 true。

   ``open`` (boolean, optional)
       连接无密码的开放网络。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``；失败时为
     ``{"result":"fail","error":"…"}``

GET /api/rules · POST /api/rules/import
---------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/rules · POST /api/rules/import``

GET 返回 rules.json 全文。POST 从 JSON body 或 ``url`` 字段（HTTP 地址） 导入规则。
内联 body 可带整份文档（最大 128 KiB）；``url`` 必须是 ``http`` 或 ``https``，且
短于 512 字符。点位 ``when`` / ``if`` 推荐 ``parameter`` + ``op`` + ``value`` （见
:doc:`../rules`）。

GET /api/scenes · POST /api/scenes/import · POST /api/scenes/invoke
-------------------------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/scenes · POST /api/scenes/import · POST /api/scenes/invoke``

GET 返回 scenes.json。import 同规则。invoke 请求体 scene_id（必填，见 :doc:`../data_model`），
按场景 ``steps`` 串行执行：步内动作按顺序下发（等价 POST /api/command），每步可
``loop`` 重复并带 ``delay_after_ms`` 延时；``max_duration_ms`` 为安全上限。场景在
独立 worker 任务里运行，新的 invoke 或配置重载会取消正在执行的场景；成功 **HTTP 200**。

GET /api/events
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/events``

近期 Hub 事件日志（HTTP 拉取；非 WebSocket）。``event_type`` 与
``GET /ws/events`` 相同。

GET /api/update/notes
---------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/update/notes``

已登录可读。返回本机 Host 固件版本、固件说明、Web 版本和 Web 说明。
不访问部署服务器。没有缓存或本地文件时，对应字段为 ``""``，响应仍是 **HTTP 200**。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``firmware_version``、 ``firmware_note``、 ``web_version``、 ``web_note``

检查与安装更新
--------------

.. container:: mh-api-block mh-api-command

   ``GET /api/update/hints · POST /api/update/check · GET /api/update/check · POST /api/update/apply``

这些是 ``系统`` 页更新角标背后的 REST 调用。``hints`` 只返回缓存的更新项：
``firmware``、``web``、``assets`` （对象或 ``null``）与 ``plugins`` （数组），
不会发起任何请求。``POST /api/update/check`` 启动后台探测并回
``{"result":"pending"}``，已有探测在跑时回 ``"unavailable"``；
``GET /api/update/check`` 报 ``"pending"`` 或 ``"idle"``。``apply`` 请求体为
``{"type":"firmware"|"asset"|"web","force":false}`` （上限 256 字节），从配置的
商店根地址取内容；``force`` 表示重装当前版本，而不是跳过它。

固件索引或文件列表在请求内取回，因此 ``apply`` 可能耗时数秒。
``type: firmware`` 只启动 ``POST /api/ota`` 一节描述的后台 OTA 任务；
``asset`` 与 ``web`` 在应答前就把文件写完。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — apply 返回 ``{"result":"ok","pending":0}``；这里的计数恒为 0，
     因为该字段报的是 apply 结果，不是排队条数
   * **HTTP 400** — 未知 ``type``、空请求体或 JSON 无效
   * **HTTP 500** — 未配置部署 URL，或下载/安装失败

GET /api/ota · POST /api/ota · POST /api/ota/upload
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/ota · POST /api/ota · POST /api/ota/upload``

GET 返回 OTA 任务状态。POST 请求体 url（必填，HTTPS 固件 .bin URL），成功 **HTTP 200**
``{"result":"started"}``；进行中再次请求 **409**。upload 为带 ``Content-Length`` 的原始
``.bin``，不在 MCP ``hub.api`` 白名单。运维章节见 :doc:`../ota`。工厂分区见
:doc:`../production`。

协议栈 REST（Kconfig 门控）
---------------------------

仅当对应协议栈编译进 Host 时存在；JSON 示例见 :doc:`../matter`、 :doc:`../zigbee`、
:doc:`../ble_mesh`。

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - 前缀
     - 用途
   * - /api/zigbee/*
     - 入网、install code、设备 upsert
   * - /api/ble_mesh/*
     - 配网许可、设备 upsert
   * - /api/matter/*
     - 控制器状态、配网、设备列表
   * - /api/thread/*
     - ``POST /api/thread/start`` 与 ``/api/thread/stop``，以及 Operational
       Dataset（``/api/thread/dataset``）和 ``GET /api/thread/status``
   * - /api/radio/provision
     - 射频/配网共存状态

登录 REST
---------

打开 Web 登录时，下列接口公开。``GET /api/auth/status`` 返回 JSON
``login_auth``、 ``password_set``、 ``logged_in``。首次：
``POST /api/auth/setup``，JSON ``username`` （须为 ``admin`` ）与
``password`` （6–64 位）。之后 ``POST /api/auth/login`` 字段相同；
``POST /api/auth/logout`` 清除 Cookie。``POST /api/auth/change`` 接收
``old_password`` 与 ``new_password`` （6–64 位），旧密码不对时回 **401**。
换皮见 :doc:`../web_console`。

可直接复制（默认开启登录）。``-c`` 保存 ``mh_sess``，后续请求用 ``-b`` 带上。
首次设密码用 ``setup`` （已设过返回 409）；之后用 ``login``。

.. code-block:: bash

   curl -s -c /tmp/mh_cookie -X POST http://HUB/api/auth/login \
     -H 'Content-Type: application/json' \
     -d '{"username":"admin","password":"secret1"}'
   curl -s -b /tmp/mh_cookie http://HUB/api/system

若设置了 ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN``，用
``-H "Authorization: Bearer TOKEN"`` 代替 Cookie 文件。

PUT /api/dev/web/
-----------------

.. container:: mh-api-block mh-api-command

   ``PUT /api/dev/web/`` *相对路径*

把 Host 控制台文件写入 ``/data/web/`` （最大 512 KiB）。例如
``PUT /api/dev/web/login.html``。若已开登录或 Bearer，鉴权与其它写入相同。操作步骤见
:doc:`../web_console`。

GET / PUT /api/dev/catalog/
---------------------------

.. container:: mh-api-block mh-api-command

   ``GET`` / ``PUT /api/dev/catalog/devices.csv``

   ``GET`` / ``PUT /api/dev/catalog/parameters.csv``

需 ``CONFIG_MINIHUB_WEB_UI``。PUT 把模型 CSV 写到 ``/data/config/model/``
（最大 128 KiB）。**两份都齐** 后扁平化为 ``/data/config/devices.csv`` 并
reload 控件表（``rebuilt: true``）。reload 是原子的（坏 CSV 保留旧表；损坏行
跳过）。GET 返回已上传的模型文件，不是扁平文件。

本机面板：``python3 tools/dev_sync.py --open``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200 GET** — ``text/csv``
   * **HTTP 200 PUT** — JSON ``result``、``file``、``rebuilt``、``bytes``
   * **HTTP 400** — 文件名不对（仅上述两个）或 CSV 无效
   * **HTTP 404 GET** — 该模型文件尚未上传

GET /ws/events
--------------

.. container:: mh-api-block mh-api-command

   ``GET /ws/events``

WebSocket 长连接，订阅 Hub 事件总线。每帧至少含 ``event_type`` 与
``timestamp_ms``。``point.updated`` 再补上 ``point_index``、``old_value``、
``new_value``；其余类型把发布者传入的 JSON 放在 ``data`` 里，发布者没带 JSON
就没有 ``data`` 键。总线（``minihub_event_bus_publish``）上发布的
**任意** 名称都会转发，插件与 OEM 固件可自定义类型。

.. only:: not hide_l3

   L3 模块用 ``event_publish`` 发布。

Host 发布的 ``event_type``：

.. list-table::
   :header-rows: 1
   :widths: 30 34 36

   * - ``event_type``
     - 时机
     - 额外字段
   * - ``point.updated``
     - 点位的 ``value`` 字符串或 ``props`` JSON 变了（``minihub_point_set``
       与 merge 系列）。规则监听此类型。
     - ``point_index``、``old_value``、``new_value``。帧里没有 ``data``，
       因此不带 ``point_id``：要用 ``GET /api/points`` 把这个索引对上点位；
       它是注册表里的位置编号，删点后会整体前移。
   * - ``point.registered``
     - 新增点位。重复登记已存在的 ``point_id`` 只原地更新，不发事件。
     - ``data``：``point_id``、``type``、``adapter``
   * - ``point.unregistered``
     - 点位被移除，一个点一帧，重载或 ``minihub_point_unregister_by_adapter``
       的批量清理也是如此。
     - ``data``：``point_id``
   * - ``command.invoked``
     - 命令下发给适配器处理函数 **之前** 发布，所以它不代表命令成功。点位或
       适配器处理函数缺失时不发布。
     - ``data``：``point_id``、``cmd``
   * - ``adapter.loaded``
     - 适配器 ``init`` 与 ``start`` 都成功、命令处理函数已注册。
     - ``data``：``adapter``
   * - ``radio.permit_closed``
     - Zigbee 允许入网窗口关闭。
     - ``data``：``adapter`` （``zigbee``）、``reason`` （协议栈给出时）
   * - ``hub.reloaded``
     - 一次软重载结束，包括 install、启用、停用、卸载触发的重载 —— 这些会增删
       点位。
     - ``data``：``ok``
   * - ``store.module.installed``
     - 一个 ``.mhub`` 安装或升级完成。
     - ``data``：``id``、``type``、``upgrade``
   * - ``device_widgets.updated``
     - 某个设备类型的图标或参数样式同步完成。
     - ``data``：``device``
   * - ``hub.ready``
     - Host 启动完成。
     - 无

.. container:: mh-api-block mh-api-attention

   **Attention**

   ``point.updated`` 会被合并。某个点位上次发布之后不到
   ``CONFIG_MINIHUB_POINT_DEBOUNCE_MS`` （默认 1000，0 关闭）时，新的写入只更新
   存储值而不发事件；窗口过后由收尾扫描补发最终值。所以这条流推的是**稳定后的
   值**，不是每一次跳变；历史记录的规则与它一致 （见上文
   ``GET /api/points/history``）。

ESP-IDF 的配网管理器会发 ``wifi.provisioning``、``wifi.provision_failed``、
``wifi.provisioned``。出厂构建改为从开放热点 ``PROV_MiniHub`` 上配 Wi-Fi，所以
这三条只在调了 ``minihub_provision_start()`` 的产品上出现 （见
:doc:`../networking`）。

L2 JS ``host.event.subscribe`` 可传事件名或 ``*`` （全部事件）。Host 侧没有
过滤名单：订阅你需要的类型即可。

.. container:: mh-api-block mh-api-attention

   **Attention**

   插件不应自行注册 httpd URI；发布点位后由 Host 转发事件。与 GET /api/events 不同：后者为
   HTTP 历史拉取。

MCP 代理
--------

局域网 Agent 可通过 :doc:`mcp` 的 hub.api 访问固定子集；该网关有自己的处理分支，
白名单与本页注册的路由不是同一集合。权威 URI 列表见
``components/minihub_web/minihub_web.c`` 中的 ``minihub_web_attach()``。


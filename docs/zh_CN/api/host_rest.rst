Host REST API
=============

:link_to_translation:`en:[English]`

概述
----

Host Web 栈对外提供的 HTTP JSON API，供 OEM Host、局域网集成、自动化与运维使用。
默认端口 **80**。WebSocket GET /ws/events 与规则引擎共用同一事件总线。

实现位于 components/minihub_web/minihub_web.c。将示例中的 HUB 替换为设备
IPv4（日志 eth got IP / wifi got IP）。

Store 插件安装与 RPC 见 :doc:`plugin_rest`；C 侧命令路由见 :doc:`minihub_core`。

通用约定
--------

* 请求/响应体为 JSON（OTA 上传为 multipart 除外）。
* 打开 ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认）时，使用 ``mh_sess`` Cookie 或
  ``Authorization: Bearer`` ，否则 **401** 。公开接口：
  ``GET /api/auth/status`` 、 ``POST /api/auth/setup`` 、 ``POST /api/auth/login`` 。
  可选 token： ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` （NVS ``api_token`` ）。
* 许多写操作成功时返回 ``{"result":"ok"}``；失败时为 HTTP 状态码 + 纯文本或 JSON。


URI 在 minihub_web_attach 中注册（components/minihub_web/minihub_web.c）。

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
   * **HTTP 404** — minihub_command_invoke 失败；body 为 esp_err_to_name
   * **HTTP 401** — 启用 auth 时未鉴权

.. container:: mh-api-block mh-api-attention

   **Attention**

   params 形状因适配器而异。点位变更经 WebSocket point.updated 推送（见 GET /ws/events）。

GET /api/points
---------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points``

返回当前点位注册表快照。

.. container:: mh-api-block mh-api-parameters

   **Parameters（查询串）**

   ``type`` (string, optional)
       按 type 过滤（如 light、sensor）。

   ``adapter`` (string, optional)
       按所属适配器名过滤。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 数组：point_id、type、value、adapter、online，以及
     ``props`` （适配器填了则含 ``params``）

.. container:: mh-api-block mh-api-attention

   **Attention**

   字段上限见 :doc:`minihub_core` 中的容量宏。一台设备一条 point；实时参数在
   ``props.params``。类型文案/控件：``GET /api/device_widgets``。

GET /api/device_widgets
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/device_widgets``

类型目录，供设备页和规则编辑器。不是实例状态。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"devices":[…]}``。每条含 ``device`` （类型 id，如
     ``light``）、``image`` （类图标 URI 或空）、``params[]``：``parameter``、
     ``label``、``widget`` （``toggle`` / ``slider`` / ``dropdown`` /
     ``text`` / ``hidden``）、``image`` （恒为空，控件走 CSS）、``style``
     （JSON：min/max/step/unit/options），以及可选 ``point_id`` （已有该类型
     登记点时，取第一条）。

.. container:: mh-api-block mh-api-attention

   **Attention**

   目录与协议无关。传输 ``point_id`` 来自登记（``协议.设备``）。隐藏参数用实例
   ``props.params.名称.visible``，目录没有 visible 列。表结构见
   ``components/minihub_device_widget/README_CN.md``。

POST /api/devices/control
-------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/devices/control``

设备页写入：解析已登记点，再走与 ``POST /api/command`` 相同的命令路径。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON body）**

   ``device`` (string)
       数据模型类型（``light``、``switch`` 等）。

   ``parameter`` (string)
       目录参数名（``power``、``brightness`` 等）。

   ``value`` (string 或 JSON)
       新值。

   ``point_id`` (string, optional)
       具体实例（如 ``sim.kitchen_lamp``）。若存在且该点的 ``type`` 与
       ``device`` 一致，Host 优先用它；否则回退到 ``(类型, 参数)`` 的首次匹配。

   ``seq`` (integer, optional)
       上报序号。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 命令成功
   * **HTTP 400** — 空 / 截断 body、超过 1024 字节、JSON 无效、缺字段、或
     ``value`` 过长
   * **HTTP 404** — 目录无此对，或该类型尚未登记点

.. container:: mh-api-block mh-api-attention

   **Attention**

   设备页 UI 会带上卡片的 ``point_id``，同类型多台可各自控制。省略
   ``point_id`` 时仍用最先登记的一条（兼容旧客户端）。自动化也可用带明确
   ``point_id`` 的 ``POST /api/command``。Host 会把 ``value`` JSON 序列化
   （字符串带引号）。数字用 JSON 数字；非数字下拉项用字符串。

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
       镜像路径或 URL。省略时优先把 ``props.ota.file`` 写入命令 ``path``，
       否则用 ``props.ota.url``。Matter 点走 CSA Provider / Requestor / BDX
       （须为带 CSA 头的 Matter OTA 镜像）。Zigbee / BLE Mesh 空中路径仍可能
       返回 ``ESP_ERR_NOT_SUPPORTED``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 已接受
   * **HTTP 400** — 缺 ``point_id``，或适配器返回 ``ESP_ERR_NOT_SUPPORTED``
   * **HTTP 404** — 未知点位
   * **HTTP 500** — 其他适配器错误

GET /api/points/history
-----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/points/history``

返回单点位的近期变更历史（Hub RAM 环形缓冲）。

.. container:: mh-api-block mh-api-parameters

   **Parameters（查询串）**

   ``point_id`` (string, required)
       目标点位。

   ``limit`` (integer, optional)
       条数上限（默认约 20；MCP 最大 50）。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 数组；元素含时间戳与值变更字段。

POST /api/points/remove
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/points/remove``

从注册表移除点位；若配置中存在则同步更新持久化。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``point_id`` (string, required)
       要移除的点位 id。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — 点位不存在

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

软重载 Hub：重读 JSON、重载 L1 包与 Store 模块、重启适配器与 JS，不整机关机。
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

   * **HTTP 200** — JSON；常见字段 uptime、version / features、ip / network（因 Kconfig 而异）

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

GET /api/rules · POST /api/rules/import
---------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/rules · POST /api/rules/import``

GET 返回 rules.json 全文。POST 从 JSON body 或 ``url`` 字段（HTTP 地址） 导入规则。
点位 ``when`` / ``if`` 推荐 ``parameter`` + ``op`` + ``value`` （见
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
不访问部署服务器。没有缓存或本地文件时，对应字段为 ``""`` ，响应仍是 **HTTP 200** 。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``firmware_version`` 、 ``firmware_note`` 、 ``web_version`` 、 ``web_note``

GET /api/ota · POST /api/ota · POST /api/ota/upload
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/ota · POST /api/ota · POST /api/ota/upload``

GET 返回 OTA 任务状态。POST 请求体 url（必填，HTTPS 固件 .bin URL），成功 **HTTP 200**
``{"result":"started"}`` ；进行中再次请求 **409** 。upload 为带 ``Content-Length`` 的原始
``.bin`` ，不在 MCP ``hub.api`` 白名单。运维章节见 :doc:`../ota` 。工厂分区见
:doc:`../production` 。

协议栈 REST（Kconfig 门控）
---------------------------

仅当对应协议栈编译进 Host 时存在；JSON 示例见 :doc:`../matter` 、 :doc:`../zigbee` 、
:doc:`../ble_mesh` 。

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
``login_auth`` 、 ``password_set`` 、 ``logged_in`` 。首次：
``POST /api/auth/setup`` ，JSON ``username`` （须为 ``admin`` ）与
``password`` （6–64 位）。之后 ``POST /api/auth/login`` 字段相同；
``POST /api/auth/logout`` 清除 Cookie。换皮见 :doc:`../web_console` 。

可直接复制（默认开启登录）。``-c`` 保存 ``mh_sess`` ，后续请求用 ``-b`` 带上。
首次设密码用 ``setup`` （已设过返回 409）；之后用 ``login`` 。

.. code-block:: bash

   curl -s -c /tmp/mh_cookie -X POST http://HUB/api/auth/login \
     -H 'Content-Type: application/json' \
     -d '{"username":"admin","password":"secret1"}'
   curl -s -b /tmp/mh_cookie http://HUB/api/system

若设置了 ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` ，用
``-H "Authorization: Bearer TOKEN"`` 代替 Cookie 文件。

PUT /api/dev/web/
-----------------

.. container:: mh-api-block mh-api-command

   ``PUT /api/dev/web/`` *相对路径*

把 Host 控制台文件写入 ``/data/web/`` （最大 512 KiB）。例如
``PUT /api/dev/web/login.html`` 。若已开登录或 Bearer，鉴权与其它写入相同。操作步骤见
:doc:`../web_console` 。

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

WebSocket 长连接，订阅 Hub 事件总线。每帧为 JSON，含 ``event_type`` （字符串）
及载荷。总线（``minihub_event_bus_publish``）上发布的
**任意** 名称都会转发。插件与 OEM 固件可自定义类型。

.. only:: not hide_l3

   L3 模块用 ``event_publish`` 发布。

Host 发布的 ``event_type``：

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - ``event_type``
     - 时机
   * - ``point.updated``
     - 点位 value/props 变更（``minihub_point_set`` / merge）。载荷含
       ``point_id``、``value``、``props``、``old_value``、``new_value``。
       规则监听此类型。
   * - ``point.registered``
     - 新点登记（``data.point_id``）。
   * - ``point.unregistered``
     - 点被移除。
   * - ``command.invoked``
     - ``minihub_command_invoke`` 之后。
   * - ``adapter.loaded``
     - 适配器已注册/加载。
   * - ``radio.permit_closed``
     - Zigbee 允许入网窗口关闭。
   * - ``wifi.provisioning``
     - Wi-Fi 配网开始（该功能编译进固件时）。
   * - ``wifi.provision_failed``
     - 配网失败。
   * - ``wifi.provisioned``
     - 配网结束。

L2 JS ``host.event.subscribe`` 可传事件名或 ``*`` （全部事件）。Host 侧没有
过滤名单：订阅你需要的类型即可。

.. container:: mh-api-block mh-api-attention

   **Attention**

   插件不应自行注册 httpd URI；发布点位后由 Host 转发事件。与 GET /api/events 不同：后者为
   HTTP 历史拉取。

MCP 代理
--------

局域网 Agent 可通过 :doc:`mcp` 的 hub.api 调用本章多数 GET/POST（白名单子集）。
权威 URI 列表见 minihub_web_attach 注册表。


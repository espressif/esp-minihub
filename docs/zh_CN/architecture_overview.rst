端到端数据流
============

:link_to_translation:`en:[English]`

.. contents::
   :local:
   :depth: 2

分层与组件清单见 :doc:`architecture`。本页把扩展 MiniHub 时最常接触的四个子系统串起来：
**设备控件** （类型目录与 UI）、**Store 模块**、**规则** 与 **Web API**，
并展示一个数值如何从一个物理设备流转到浏览器、再回到设备。类型目录与控件见
:doc:`type_catalog`；点位与 Hub 配置见 :doc:`data_model`。

全局视图
--------

MiniHub 是一款多协议物联网中枢固件。设备上运行一个通用的 **Host** （Core +
内置适配器）；厂商 / 第三方行为以 **Store 模块** （``.mhub``）形式交付，而非
Host Core 的依赖。

.. code-block:: text

   ┌──────────────────────────────────────────────────────────────┐
   │ 浏览器  ──  Web 控制台（HTML/CSS/JS，位于 modstore LittleFS） │
   └───────────────┬───────────────────────────┬──────────────────┘
                   │ HTTPS（REST + WS）         │ WebSocket /ws/events
                   ▼                            │
   ┌──────────────────────────────────────────┼──────────────────┐
   │ minihub_web  （REST /api/*、/img/、/ws/events）              │
   ├──────────────────────────────────────────┼──────────────────┤
   │ minihub_hub  编排                                          │
   │  ├─ minihub_core        数据点注册表（一设备一个点）         │
   │  ├─ minihub_device_widget   类型目录（UI 渲染）              │
   │  ├─ minihub_rules       when/if/then 自动化                 │
   │  ├─ minihub_scenes      场景召回                            │
   │  └─ minihub_store / minihub_module / minihub_pack  Store    │
   ├──────────────────────────────────────────────────────────────┤
   │ minihub_adapters  zigbee / matter / thread / ble_mesh / …   │
   │ examples/plugins + MINIHUB_PLUGIN_DIRS  Store .mhub 源码       │
   └──────────────────────────────────────────────────────────────┘
                   │ 协议帧
                   ▼
            物理设备 / 射频

依赖方向单向向下：``minihub_web`` 依赖 ``minihub_device_widget`` 与
``minihub_core``；设备控件组件是独立的，编译期并不依赖 Core。适配器把点注册
进 Core；下层不会回调 Web。（设备侧 MCP 服务器 ``CONFIG_MINIHUB_MCP_SERVER``
与 ``tools/`` 下给插件作者用的 IDE MCP 无关。）

端到端数据流
------------

1. 发现与注册
^^^^^^^^^^^^^

   适配器（如 Zigbee、Matter、BLE Mesh）发现设备后，为每个设备注册
   **恰好一个** ``minihub_point_t`` （Matter 按 endpoint）。点 id 如
   ``zigbee.zb_7d4b``、``thread.matter_n2_ep1``、``sim.color_bulb``。
   Thread 适配器只做 Border Router，**不** 登记设备点，见 :doc:`thread`。
   实时参数、访问权限、可见性、实例上下界都存放在该点上的结构化 JSON
   里——典型为 ``props.params.<name> = { "value", 可选 access / visible / min /
   max / step / unit }``。该点还带一个 ``type`` 字符串（如 ``light``）表示
   数据模型类型。详见 :doc:`data_model`；类型目录与控件见 :doc:`type_catalog`。

2. 类型目录（设备控件）
^^^^^^^^^^^^^^^^^^^^^^^

   一个 ``type`` 如何渲染是**声明式**描述的，而非写在 C 里。权威来源是
   ``components/minihub_device_widget/model/*.csv`` （``devices.csv``、
   ``parameters.csv``、``services.csv``、``ui_elements.csv``）。构建期由
   ``components/minihub_device_widget/tools/gen_device_widget.py`` 折叠成编译
   进固件的注册表（``MINIHUB_DW_EMBEDDED_CSV``），固件再把它们扁平化为运行时
   文件 ``/data/config/devices.csv``。启动时 ``minihub_device_widget_init``
   在文件缺失时从内嵌副本 seed，再加载它。

   该目录是**协议无关**的：不含 transport 字段。适配器前缀（``zigbee.`` /
   Matter 的 ``thread.matter_n…`` 或 ``wifi.matter_n…`` 等）来自点的注册，
   因此同样的 ``light`` /
   ``switch`` 控件不论设备以何种方式接入都能渲染。内置类型表、控件与开发
   用法见 :doc:`type_catalog`。

3. 渲染
^^^^^^^

   设备页发起 ``GET /api/device_widgets`` （处理函数 ``handle_api_device_widgets``
   位于 ``components/minihub_web/minihub_web.c``）以及 ``GET /api/points``，
   随后 ``device_widgets.js`` 把类型目录与每个实时点的 ``props.params`` 求交，
   绘制 CSS 控件（``device_widgets.css``）：``toggle`` / ``slider`` /
   ``dropdown`` / ``text`` / ``hidden``。每张卡片对应一个注册点（即一个设备），
   绝不为每个参数各画一张卡片。设备头像由 ``GET /img/devices/…`` 提供。

4. 控制
^^^^^^^

   点击控件会调用 ``POST /api/devices/control`` （处理函数
   ``handle_api_devices_control``），JSON 为
   ``{ "device": 类型, "parameter": 名, "value": …, "point_id": id, "seq": n }``。
   ``device`` 是**数据模型类型** （``light``、``switch`` 等）。``point_id`` 来自
   卡片上的实时实例（如 ``sim.kitchen_lamp``）；若该点存在且类型匹配则优先用它，
   否则回退到 ``(类型, 参数)`` 的首次匹配。请求体整段读完（最大 1024 字节）；
   ``value`` 经 JSON 序列化，字符串带引号。数字下拉项仍是数字；非数字选项
   （``cool`` / ``auto``）保持字符串。自动化也可用带明确 ``point_id`` 的
   ``POST /api/command`` （可选 ``params.parameter``）。L2 JS
   ``host.command.invoke`` 为 ``{ point_id, cmd, params? }``。

5. 事件与规则
^^^^^^^^^^^^^

   每次点更新都会发出 ``point.updated`` 事件，有两个消费者：

   * 实时 UI 通过 WebSocket ``/ws/events`` （``components/minihub_web/minihub_web_ws.c``）
     刷新。事件类型见 :doc:`api/host_rest` 的 ``GET /ws/events``。
   * 规则引擎（``minihub_rules``）评估 ``when`` / ``if`` / ``then``。命中后执行
     ``command``、``command_by_type``、``delay`` 或 ``notify``
     （见 :doc:`rules`）。场景是独立的 ``POST /api/scenes/invoke``，不是规则
     ``then`` 类型。

   规则、场景与模块 RPC 都通过 REST 暴露（``/api/rules``、``/api/scenes``、
   ``/api/modules/*``）。

6. Store 模块
^^^^^^^^^^^^^

   第三方行为以 **Store 模块** 形式交付，位于 ``/store/modules/ID/``：

   * **L1** ``minihub_pack`` —— 注入已有适配器（不引入新射频协议栈）。
   * **L2** ``minihub_module`` —— JavaScript 插件（``main.js``，经 ``minihub_js``
     的 QuickJS）；可附带 ``web/``，由 ``/modules/ID/web/…`` 提供。
   * **L3** ``minihub_module`` —— 运行时加载的原生 ``.so``。

   可安装模块的目录即 **Store** （``minihub_store``）。演示放在
   ``examples/plugins/`` （经 ``MINIHUB_PLUGIN_DIRS``）；公开 Host 的
   ``store_preinstall`` 默认留空，除非有意内置出厂模块。模块订阅事件、调用
   Host API、注册点——但绝不在 Core 中写入厂商令牌。

类型目录的运行时约定
--------------------

目录可在**不重新编译固件**的情况下通过
``PUT /api/dev/catalog/{devices.csv|parameters.csv}`` 编辑（见
:doc:`api/host_rest`）。reload 路径被设计为可容错：

* **原子 reload。** 新表先完整构建，再交换内存中的注册表。若传入文件无法解析，
  则保留旧表并使请求失败——UI 永远不会读到半释放的注册表。
* **坏行跳过而非致命。** 单行格式错误会被记录并丢弃，其余好行仍加载（旧行为
  是任一行出错就整表拒绝并覆盖自定义内容）。
* **类型未登记可见。** 当某个点的 ``type`` 为空或不在目录中时，卡片会渲染一个
  “类型未登记”徽标，并在控制台打印告警，从而把数据问题与渲染问题区分开。
* **自愈。** 若启动时 ``/data/config/devices.csv`` 缺失或损坏，固件会用内嵌
  副本重新 seed 并 reload。

API 速查
--------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - 端点
     - 用途
   * - ``GET /api/points``
     - 列出已注册点（设备）及其 ``props.params``
   * - ``GET /api/points/history``
     - 单点数值历史
   * - ``POST /api/points/remove``
     - 遗忘一个点
   * - ``GET /api/device_widgets``
     - 类型目录（文案 / 控件）；可选第一条匹配的 ``point_id``
   * - ``POST /api/devices/control``
     - 设备页：``device`` 是类型；可选 ``point_id`` 指定实例
   * - ``POST /api/command``
     - 按 ``point_id`` 发命令（可选 ``params.parameter``）
   * - ``GET /api/adapters``
     - 适配器概览（启用 / 模块）；不是完整 ``adapters.json``
   * - ``GET|PUT /api/config/adapters``
     - 读取 / 替换 adapters 文档
   * - ``GET|PUT /api/dev/catalog/{devices.csv|parameters.csv}``
     - 运行时读取 / 替换类型目录
   * - ``GET /api/rules``、``POST /api/rules/import``、``POST /api/rules/ai/chat``
     - 规则管理与 AI 助手
   * - ``GET /api/modules``、``POST /api/modules``、``POST /api/modules/*``
     - Store 模块列表 / 安装 / RPC
   * - ``GET /api/modules/*/preview``
     - 模块 UI 预览
   * - ``GET /api/packs``、``POST /api/packs``
     - L1 pack 列表 / 管理
   * - ``GET /api/scenes``、``POST /api/scenes/import``、``POST /api/scenes/invoke``
     - 场景管理
   * - ``GET /api/events``、WebSocket ``/ws/events``
     - 点 / 事件流（REST 轮询 + 实时 socket）
   * - ``GET /img/`` 加子目录与文件名
     - 设备 / 参数图标（已做路径穿越防护）
   * - ``POST /api/hub/reload``
     - Hub 运行时软重载（会重跑目录 reload）

下一步
------

* :doc:`type_catalog` —— 设备类型、控件与图标。
* :doc:`data_model` —— 点位模型与 adapters / 场景。
* :doc:`rules` —— ``when`` / ``if`` / ``then`` 与比较 ``op``。
* :doc:`web_console` —— 替换或换肤 Web 控制台。
* :doc:`plugins/index` —— 编写 L1/L2/L3 Store 模块。
* :doc:`api/index` —— 完整 REST / JS / MCP / C ABI 参考。

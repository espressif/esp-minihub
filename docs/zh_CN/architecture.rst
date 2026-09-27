分层与原则
==========

:link_to_translation:`en:[English]`

分层
--------

.. code-block:: text

   +------------------------------------------------------------------+
   |  应用层                                                          |
   |  Web UI / REST / WS / MCP / Store 目录 / CLI                     |
   +------------------------------------------------------------------+
                                  |
   +------------------------------------------------------------------+
   |  MiniHub Host                                                    |
   |  minihub_web / minihub_mcp_*     Web 控制台、REST、MCP           |
   |  minihub_core / minihub_hub      点位、命令、规则、热重载        |
   |  minihub_module                  Store .mhub (L2 JS / L3 .so)    |
   +------------------------------------------------------------------+
                                  |
   +------------------------------------------------------------------+
   |  设备接入                                                        |
   |  静态适配器 (Kconfig)   Wi-Fi / Zigbee / BLE Mesh / Matter       |
   |                         Thread / MQTT / Modbus / ...             |
   |  Store 插件             厂商云 / OEM，无需维护 Host              |
   |  Host 二次开发          更省内存，可调用 IDF / MiniHub API       |
   +------------------------------------------------------------------+
                                  |
   +------------------------------------------------------------------+
   |  现场设备                                                        |
   |  灯、传感器、网关、厂商云端点                                    |
   +------------------------------------------------------------------+

   IP 上行：minihub_net — 见 :doc:`networking`。

设计原则
--------

**Core 保持协议中立。** 乐鑫维护的射频/IP 栈（Wi-Fi STA 适配器、Zigbee、BLE Mesh、
Matter、Thread、MQTT、Modbus）可以静态链入 Host。厂商云与 OEM 差异不要写进 Core，应通过 Store
插件或基于 Host 的二次开发交付。这样通用 Host 可以在不含第三方凭证的情况下编译。

**点位是唯一的自动化 ABI。** 一台物理设备一条 point（``协议.设备``）。参数在
``props.params``。适配器隐藏协议细节。规则对 ``sim.color_bulb`` 发
``turn_on`` ，日后换成其它适配器只要沿用同一 ``point_id`` 即可。

**软重载代替整机重启。** 改 ``adapters.json`` 、安装 ``.mhub`` 或开关模块都会走
``minihub_hub_reload`` — 适配器停止、模块 ``dlclose`` ，再加载。Matter+Thread
可能重挂 CHIP；重载期间 MCP 可能返回 ``HUB_NOT_READY`` 。

**两块可写 Flash 分区。** 通用 Host 把 ``storage`` 挂到 ``/data`` （Hub 配置），把
``modstore`` 挂到 ``/store`` （已装插件与 Web 控制台）。工装整机烧录会重写这两块；
只烧 ``app-flash`` 不会擦除。见 :doc:`production`。

JSON 编写：:doc:`data_model` （点位 / adapters / 规则）；类型目录与控件：
:doc:`type_catalog`。端到端流程（目录、设备页、Store）：
:doc:`architecture_overview`。

关键组件
--------

* ``minihub_core`` — 点位注册表、命令路由、事件总线
* ``minihub_config`` — 加载/保存 ``/data/config/*.json``
* ``minihub_hub`` — 软重载编排
* ``minihub_web`` — HTTP REST、Web 控制台、 ``/ws/events``
* ``minihub_module`` — L2/L3 加载、ABI / Host 虚表
* ``minihub_pack`` — L1 配置包（ ``inject`` 合并）
* ``minihub_store`` — 目录、安装/升级/卸载
* ``minihub_net`` — 以太网 / Wi-Fi STA
* ``minihub_matter`` — Matter 控制器胶水层
* ``minihub_js`` — QuickJS L2 运行时
* ``minihub_device_widget`` — 类型目录（CSV → 文案 / 控件）
* ``adapters/*`` — 静态协议栈
* ``examples/plugins/ID/`` — 仓库内 Store 演示（额外树用 ``MINIHUB_PLUGIN_DIRS``）

存储布局（通用 Host）
----------------------

默认可写根目录： ``/data`` （通用 ``minihub`` 产品上为 LittleFS）。

.. code-block:: text

   /data/config/
     adapters.json      运行时适配器与设备列表（首次启动由出厂配置复制）
     rules.json         自动化规则
     scenes.json        场景定义
     devices.csv        扁平化类型目录
     model/             可选上传的 devices.csv + parameters.csv
   /data/config/packs/  可选 L1 包暂存（因产品而异）
   /store/modules/ID/   已安装 Store 模块（module.json、.so、main.js 等）
   /store/preinstall/web/  Host Web 控制台（index.html、login.html 等）
   /data/tmp/           MHMD 解压 / 临时目录（模块加载器）

NVS 保存 Wi-Fi 凭据、Store URL 覆盖、OTA 元数据等 — 完整 ``adapters.json`` 在
LittleFS 上。

启动顺序（ ``application/minihub`` ）
-------------------------------------

典型 ``main`` 启动顺序：

1. Flash 分发器、挂载 ``/data`` 、 ``minihub_config_init()``
2. ``minihub_core_init()`` — 点位、事件总线、命令路由
3. ``minihub_net_init()`` — 启动以太网；Wi-Fi **仅准备** （延迟 STA）
4. 按构建做 Matter/Thread bootstrap（``main`` 里 CHIP / OpenThread 顺序）
5. ``minihub_module_loader_init()`` ；从 ``/store/modules/`` 加载 Store 模块
6. ``minihub_web`` HTTP 服务（射频 start 期间 UI 仍可访问）
7. ``minihub_adapter_load_all()`` + ``minihub_adapter_start_all()`` — STA 连接由
   ``wifi`` 适配器完成；若 Wi-Fi 与 Zigbee/Thread 同时开，射频互斥会跳过
   BLE Mesh（见 :doc:`radios`）。随后 MCP / 日志 ``MiniHub ready``。

日志 ``wifi STA deferred`` 表示 ``wifi`` 适配器尚未 start，属正常现象。见 :doc:`networking`。

Hub 软重载
----------

``POST /api/hub/reload`` （或 MCP ``hub.reload`` ）执行 ``minihub_hub_reload()`` ：

1. ``minihub_config_reload()`` — 从 Flash 重读 JSON
2. ``minihub_pack_reload()`` — 合并 L1 包到 adapters 视图
3. 停止全部适配器； ``minihub_module_reload()`` — 卸载 L3、重扫 manifest
4. ``minihub_adapter_load_all()`` + ``start_all()``
5. ``minihub_js_reload()`` — 重新执行 L2 ``setup(host)``
6. ``minihub_rules_reload()`` + ``minihub_scenes_reload()``

适配器生命周期锁防止在 ``dlclose`` 期间运行 ``adapter_poll`` 。Matter + Thread
构建可能在重载后重新将 CHIP 挂到 OpenThread。

数据流：命令
------------

.. code-block:: text

   REST POST /api/command
        → minihub_command_invoke
        → adapter.handle_command（静态或 Store 模块）
        → minihub_point_set / 设备 I/O
        → minihub_event_bus_publish → WebSocket /ws/events

数据流：Store 安装
------------------

.. code-block:: text

   POST /api/store/install { kind, id }
        → 下载 .mhub → 解压到 /store/modules/ID/
        → 可选 hub reload
        → minihub_module_load_all → minihub_module_entry → 注册适配器

见 :doc:`plugins/pack_and_catalog`、:doc:`api/host_rest`、:doc:`api/plugin_rest`。

配置来源
--------

* **构建期：** Kconfig、 ``manifest.yml`` 、 ``sdkconfig_features``
* **出厂配置：** ``application/APP/config/adapters.json`` 或仓库 ``config/``
* **运行时：** Web UI、REST ``PUT /api/config/adapters`` 、
  ``PUT /api/dev/catalog/…`` 、通过 Host 虚表
  ``config_save_adapters`` 的模块 RPC

生成文件（勿手改）
------------------------

``sync_host_manifest.py`` 生成：

* ``application/APP/main/idf_component.yml``
* ``application/APP/main/adapters_setup.generated.c``
* ``cmake/generated/minihub_components.cmake``

见 :doc:`application/index` 再 :doc:`host_customization`。

二次开发公开契约
----------------

以下为 OEM Host 与 Store 插件的受支持扩展面。

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - 接口面
     - 章节 / 产物
   * - 产品 profile
     - ``manifest.yml`` 、 ``partitions.csv`` 、 ``config/sdkconfig/app.yml`` —
       :doc:`host_customization`
   * - 运行时 JSON / 类型目录
     - ``adapters.json`` 、 ``rules.json`` 、 ``scenes.json`` —
       :doc:`data_model`；类型目录 CSV — :doc:`type_catalog`
   * - HTTP / 事件
     - :doc:`api/host_rest`；WebSocket 事件 ``/ws/events``
   * - MCP 工具
     - :doc:`api/mcp`
   * - L2 JS host 对象
     - :doc:`api/js_host`
   * - L3 模块与适配器 C API
     - :doc:`api/module_abi`、:doc:`api/minihub_core`、:doc:`api/minihub_adapter`
   * - L3 链接符号
     - ``docs/module_sdk/host_export_allowlist.json``
   * - Store 打包 / 目录
     - :doc:`plugins/pack_and_catalog`
   * - 工厂 / OTA
     - :doc:`production`

ABI 稳定性：L3 原生模块在 ``module.json`` 中声明 ``abi_version`` 、
``core_api_min`` 与 ``core_api_max`` 。每次发布应配套固件与 mhub 包（见 :doc:`production`）。
Host 导出符号变更须更新 allowlist 并重新生成符号（:doc:`plugins/develop_l3`）。

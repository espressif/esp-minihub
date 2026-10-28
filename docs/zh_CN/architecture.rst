分层与原则
==========

:link_to_translation:`en:[English]`

设备上跑着什么、哪一层允许依赖哪一层、从上电到第一次点位更新之间发生了什么。
如果要跟着一次控制走完整条回路，读 :doc:`data_flow`。

分层
--------

.. image:: ../_static/architecture/layered.svg
   :align: center
   :alt: MiniHub 分层由外向内：入口、Hub 服务、两朵云、不认协议的核心层、运行时扩展、协议侧、芯片与构建；右边缘的紫色通道是两种向上的扩展机制

命令从控制台、REST 或 MCP 进来，Host Core 把它落到一条点上，适配器或商店模块去说
协议，产生的 ``point.updated`` 沿同一条路回到 UI、规则引擎与 MCP 客户端。同一带里的
目录名，都相对带标题右半边那个前缀。

有两条边是朝上的。 ``minihub_net`` 自己定义了 ``minihub_web_refresh_geo_async()`` 、
``minihub_lcd_request_refresh()`` 与 ``minihub_rainmaker_notify_got_ip()`` 三个 weak
空桩，并在拿到 IP 时调用它们；链进这些组件的构建会把空桩替换掉，而网络层不需要知道
它们的名字。装进来的代码能调的那些 Host 入口，是 ``minihub_module`` 里一张虚表交出去
的，不是靠依赖它换来的。

一个协议通常是两个目录 —— 栈和它的适配器 —— 两者不在同一层：MQTT 与 Modbus 适配器
只调用 Core，Zigbee 与 BLE Mesh 还要调用自己的栈和加载器。

两种云：一个后装，一个链进镜像
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Hub 最终会说上话的云有两种，两者的差别就在「说」的代码是怎么来的：

* **厂商云是装进来的。** MIoT 之类以一个商店模块的形态到来：要么是被合并进
  ``adapters.json`` 的 L1 包，要么是自己登录、订阅、上报的 L2 模块。它在一台已经
  跑着的 Hub 上安装、升级、卸载 —— 厂商专有的代码因此不链进 Host：把模块卸载，
  这条路就没了。
* **RainMaker 是编译进来的。** ``components/minihub_rainmaker`` 由
  ``CONFIG_MINIHUB_RAINMAKER`` （缺省关）决定，开了就把 ESP-IDF 那座桥链进镜像，
  所以这朵云只存在于开了它的构建里，要改它等于重新构建再走一次 OTA。

适配器和模块要说话的对象里，除了云还有子设备（Zigbee、BLE Mesh、Matter over
Thread）、站点自己跑的 MQTT broker 与部署服务器 —— 它们同样不是某一层，各自写在
实现它们的章节里（ :doc:`radios` 、 :doc:`networking` 、 :doc:`production` ）。

两种都不由 Host Core 去说云：Core 只拥有点位与事件总线，把点位翻成某个云自己的
设备/服务语汇，发生在内置那座桥或后装的那个模块里。所以换一家云不会改到 Core。

IP 上行：``minihub_net`` —— 见 :doc:`networking`。

`交互版 <_static/architecture/index.html>`_ —— 各层自上而下自动导览（悬停暂停）；
悬停一层看它的约束，点击一个方块即可单独留下它，并读出它被禁止做什么。

.. only:: not hide_l3

   ``minihub_module`` 同时加载 L3 ``.so`` 包（以及 Hybrid）。

设计原则
--------

Core 不认协议。不需要厂商账号或服务端就能工作的射频/IP 栈（Wi-Fi STA 适配器、
Zigbee、BLE Mesh、Matter、Thread、MQTT、Modbus）可以静态链入 Host。凡是要靠厂商账号
或服务端才跑得通的，都不写进 Core，通过商店插件或基于 Host 的二次开发交付。所以
编译通用 Host 时不需要任何第三方凭证。

自动化只认点位。一台物理设备一条 point（ ``协议.设备`` ），参数放在
``props.params``，协议细节由适配器自己挡住。规则对 ``sim.js_kitchen`` 发
``turn_on``，日后这台设备走别的适配器，只要 ``point_id`` 沿用，规则不用改。

改配置是热重载，不重启整机。改 ``adapters.json``、装 ``.mhub`` 或开关模块都会调
``minihub_hub_reload``：适配器先停再加载。Matter+Thread 构建可能要把 CHIP 重挂到
OpenThread，所以得等 Hub ready（重载期间 MCP 返回 ``HUB_NOT_READY`` ）。

.. only:: not hide_l3

   重载时也会 ``dlclose`` L3 模块，然后再加载。

通用 Host 有两块可写分区。 ``storage`` 挂在 ``/data`` （Hub 配置 **与**  Web
控制台，控制台在 ``/data/web/``，设备图标在 ``/data/images/``）。第二块
``modstore`` 挂在 ``/store`` ，存放商店目录树：已装模块、包与 ``catalog.json``
都在这里。它只在 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 打开时才挂载；关掉该选项就没有可写的
商店根目录，商店下载也随之禁用。
``/store/preinstall/web/`` 是仍在兜底查找的遗留控制台位置。工装整机烧录会重写这两块，只烧
``app-flash`` 不会擦除。见 :doc:`production`。

JSON 编写：:doc:`data_model` （点位 / adapters / 规则）；类型目录与控件：
:doc:`type_catalog`。端到端流程（目录、概览页、商店）：
:doc:`data_flow`。

关键组件
--------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 组件
     - 运行时它管什么
   * - ``minihub_core``
     - 点位注册表、命令路由、事件总线
   * - ``minihub_config``
     - 读写 ``/data/config/*.json``
   * - ``minihub_hub``
     - 热重载的那套顺序
   * - ``minihub_web``
     - HTTP REST、Web 控制台、 ``/ws/events``
   * - ``minihub_module``
     - 读清单、加载模块
   * - ``minihub_pack``
     - L1 配置包，经 ``inject`` 合并
   * - ``minihub_store``
     - 目录、安装、升级、卸载
   * - ``minihub_net``
     - 以太网与 Wi-Fi STA
   * - ``minihub_matter``
     - Matter 控制器胶水层
   * - ``minihub_js``
     - QuickJS 工作线程与 ``host`` 对象
   * - ``minihub_device_widget``
     - 类型目录：进 CSV，出文案与控件
   * - ``minihub_adapter``
     - 适配器注册表、射频启动顺序、生命周期锁、轮询任务
   * - ``minihub_protocol_adapters/adapter_*``
     - 静态协议栈（wifi、zigbee、ble_mesh、matter、thread、mqtt、modbus）
   * - ``examples/plugins/ID/``
     - 仓库内的商店演示；额外源码树走 ``MINIHUB_PLUGIN_DIRS``

.. only:: not hide_l3

   ``minihub_module`` 也通过 ABI 虚表加载 L3 ``.so`` 模块。

哪个目录放着什么、该由谁去改，见 :doc:`source_tree`。

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
   /store/modules/ID/   已安装 商店 模块（module.json、main.js 等）
   /data/web/           Host Web 控制台（index.html、login.html 等）
   /data/tmp/           临时目录（模块加载器）

NVS 保存 Wi-Fi 凭据、商店 URL 覆盖、Web 密码与 API token。OTA 状态（启动槽、
自检待确认）在 ``otadata`` 分区，不在 NVS。完整 ``adapters.json`` 两者都不存 ——
它在 LittleFS 上。

启动顺序（ ``application/minihub`` ）
-------------------------------------

``main`` 里的实际顺序 （每个一步就是一行调用）：

1. ``minihub_auth_init()``、 ``minihub_ota_init()``，然后 ``minihub_core_init()``
   — 点位注册表、事件总线、命令路由
2. 按 Kconfig 决定的是否初始化板载 MCP
3. ``minihub_config_init()`` — 挂载 ``/data`` 与 ``/store`` 的就是这一步
4. 适配器注册表初始化 + 静态适配器注册；随后模块加载器初始化、
   ``minihub_module_load_all()``、包初始化与合并
5. ``minihub_adapter_load_all()`` — 网络起来之前先把 manifest 读完
6. ``minihub_net_init()`` — 启动以太网；Wi-Fi **仅准备** （延迟 STA）
7. 按构建执行 ``minihub_thread_init()``，加载规则与场景
8. ``minihub_web_start(80)`` — 射频 start 期间 UI 仍可访问
9. ``minihub_adapter_start_all()`` — STA 连接由 ``wifi`` 适配器完成；若 Wi-Fi 或
   BLE Mesh 与 Zigbee/Thread 同时开，射频互斥会跳过 802.15.4 那个适配器
   （见 :doc:`radios`）
10. Matter↔Thread 挂接、 ``minihub_js_reload()`` 与 Hybrid 的延迟加载，接着按构建
    执行 USB MSC / RTSP / RainMaker / LCD，再起适配器轮询任务，最后打印
    ``MiniHub ready``。

日志 ``wifi STA deferred`` 表示 ``wifi`` 适配器尚未 start，属正常现象。见 :doc:`networking`。

Hub 软重载
----------

``POST /api/hub/reload`` 同步执行 ``minihub_hub_reload()``；MCP ``hub.reload``、
商店安装、``adapters.json`` 写入都走 ``minihub_hub_reload_async()``，在
``hub_reload`` 任务上跑，并有一把 60 秒的锁让第二次重载排队等待。
``minihub_hub_reload()`` 的实际步骤：

1. 先 ``minihub_js_unload_all()`` — L2 回调必须在地基被拆掉之前停止碰点位
2. 适配器生命周期锁（60 秒；拿不到就 ``ESP_ERR_TIMEOUT`` 中止本次重载）
3. ``minihub_config_reload()`` — 从 Flash 重读 JSON（出错即中止）
4. ``minihub_pack_reload()`` — 合并 L1 包到 adapters 视图
5. 停止全部适配器； ``minihub_module_reload()`` — 重扫 manifest
6. ``minihub_adapter_load_all()`` + ``minihub_adapter_start_all()``
7. 解锁；Matter + Thread 构建把 CHIP 重新挂到 OpenThread
8. ``minihub_js_unload_all()`` + ``minihub_js_reload()`` （重跑 ``setup(host)``），
   随后 ``minihub_module_load_deferred()`` 补 Hybrid 包
9. ``minihub_rules_reload()`` + ``minihub_scenes_reload()``，最后在事件总线发
   ``hub.reloaded`` — 控制台就是靠它重取数据，而不是相信那次 HTTP 应答。

同一把生命周期锁也保证重载期间不会跑 ``adapter_poll``。

.. only:: not hide_l3

   同一把锁也覆盖 L3 ``.so`` 的 ``dlclose``。

数据流：命令
------------

.. code-block:: text

   REST POST /api/command
        → minihub_command_invoke
        → adapter.handle_command（静态或 商店 模块）
        → minihub_point_set / 设备 I/O
        → minihub_event_bus_publish → WebSocket /ws/events

数据流：商店安装
------------------

.. code-block:: text

   POST /api/store/install { type, id }
        → 下载 .mhub → 解压到 /store/modules/ID/
        → 可选 hub reload
        → minihub_module_load_all → minihub_module_entry → 注册适配器

见 :doc:`plugins/pack_and_catalog`、:doc:`api/host_rest`、:doc:`api/plugin_rest`。

配置来源
--------

* **构建期：** Kconfig、 ``manifest.yml``、 ``sdkconfig_features``
* **出厂配置：** ``application/APP/configs/adapters.json`` 或仓库 ``configs/``
* **运行时：** Web UI、REST ``PUT /api/config/adapters``、
  ``PUT /api/dev/catalog/…``、通过 Host 虚表
  ``config_save_adapters`` 的模块 RPC

二次开发公开契约
----------------

以下为自定义开发 Host 固件与商店插件的受支持扩展面。

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - 接口面
     - 章节 / 产物
   * - 产品 profile
     - ``manifest.yml``、 ``partitions.csv``、 ``configs/sdkconfig/app.yml`` —
       :doc:`host_customization`
   * - 运行时 JSON / 类型目录
     - ``adapters.json``、 ``rules.json``、 ``scenes.json`` —
       :doc:`data_model`；类型目录 CSV — :doc:`type_catalog`
   * - HTTP / 事件
     - :doc:`api/host_rest`；WebSocket 事件 ``/ws/events``
   * - MCP 工具
     - :doc:`api/mcp`
   * - L2 JS host 对象
     - :doc:`api/js_host`
   * - 商店打包 / 目录
     - :doc:`plugins/pack_and_catalog`
   * - 工厂 / OTA
     - :doc:`production`

.. only:: not hide_l3

   .. list-table::
      :header-rows: 1
      :widths: 28 72

      * - 接口
        - 章节 / 产物
      * - L3 模块与适配器 C API
        - :doc:`api/module_abi`、:doc:`api/minihub_core`、:doc:`api/minihub_adapter`
      * - L3 链接符号
        - ``docs/module_sdk/host_export_allowlist.json``

   ABI 稳定性：L3 原生模块在 ``module.json`` 中声明 ``abi_version``、
   ``core_api_min`` 与 ``core_api_max``，Host 会拒绝安装或加载不匹配的那一个。
   原生模块不发布到商店，因此一次发布交付的是 Host 固件加 L1/L2 包、资源与 Web
   控制台。
   Host 导出符号变更须更新 allowlist 并重新生成符号（:doc:`plugins/develop_l3`）。

仓库布局
========

:link_to_translation:`en:[English]`

.. contents::
   :local:
   :depth: 2

顶层
----

.. code-block:: text

   application/       一个目录一个产品；这是你烧的东西
   components/        Core、服务、协议适配器
   configs/           共享出厂 JSON 与构建期默认值
   cmake/             构建胶合：产品包、模块 .so 规则、资源
   tools/             PC 侧脚本（构建、打包、分发、文档）
   examples/plugins/  仓库内的 商店 模块源码，仅供参考
   docs/              本指南，以及面向插件的 SDK 面
   README.md

.. list-table::
   :header-rows: 1
   :widths: 20 44 36

   * - 路径
     - 里面是什么
     - 何时打开
   * - ``application/``
     - 产品：板级、分区表、sdkconfig 配方、出厂 JSON、 ``app_main``。目录名
       就是产品名。
     - 你要出自己的 SKU（ :doc:`application/index`、
       :doc:`host_customization` ）
   * - ``components/``
     - 共享 Core、服务与适配器。每个产品都链接它们，不要复制进产品目录。
     - 你要改 Host 本身的行为
   * - ``configs/``
     - ``minihub_manifest.yml`` （所有产品 ``extends: _host_base`` 的共享
       manifest 基座）、 ``adapters.json``、 ``rules.json``、
       ``scenes.json``、 ``sdkconfig/``、 ``product_params/``。
     - 某个默认值应该对所有产品都变
   * - ``cmake/``
     - ``minihub_bundle/`` （产品引入的 include 与 Kconfig 胶合）、
       ``minihub_module_so.cmake`` 与 ``minihub_plugin_standalone.cmake``
       （原生模块构建）、 ``minihub_product_assets.cmake``、
       ``matter_no_rtti.cmake``、 ``check_shared_object.cmake``。
     - 一般不碰；产品配方能覆盖大部分需求
   * - ``tools/``
     - 跑在你 PC 上、不跑在 Hub 上的脚本。
     - 打包、分发、生成
   * - ``examples/plugins/``
     - 演示用商店模块（ ``color_bulb_js``、 ``meter_plug_js``、
       ``ble_mesh_pack`` ）。你自己的插件仓库可以通过 ``MINIHUB_PLUGIN_DIRS`` 挂在
       旁边。
     - 你想要一个可以直接抄的起点
   * - ``docs/``
     - ``en/`` 与 ``zh_CN/`` （本指南，逐文件对等）、 ``module_sdk/`` （插件
       可见的头文件集、模板、导出符号白名单）、 ``_extensions/`` 与
       ``_static/`` （构建辅助）。
     - 指南里有东西写错或缺了

``components/`` 按职责
----------------------

.. image:: ../_static/source_tree/component_map.svg
   :align: center
   :alt: MiniHub 组件按职责分成五行：核心与服务、设备模型与界面、商店与模块、射频、可选硬件

Core 与服务
    这一组撑着 Hub 本身：点位注册表、它要读的文件、以及它说话用的链路。

    点位注册表、命令路由、事件总线和工作任务在 ``minihub_core`` ；
    ``/data/config/*.json`` 的读写在 ``minihub_config`` ；热重载那套顺序在
    ``minihub_hub`` ；控制台会话与可选的 API token 在 ``minihub_auth`` ；收网络凭据
    （ SoftAP ）与恢复出厂在 ``minihub_provision`` ；Ethernet 与 Wi-Fi STA 的拉起在
    ``minihub_net`` ；固件升级的状态与进度在 ``minihub_ota`` ；把固件暂存到外部介质、
    交接 Vibe Mode 在 ``minihub_vibe`` （Host 库，外加嵌套的
    ``subproject/`` 产出保留的 ``app,factory`` 镜像 —— 不是 ``application/`` 下的产品），从部署根拉取并应用
    固件、资源与控制台包在 ``minihub_update`` ；站点里的 MQTT 设备连的那个内置
    broker 在 ``minihub_mqtt_broker`` 。

设备模型与 UI
    这一组组件决定设备在 UI 中的呈现方式，以及自动化的触发时机。

    类型目录（ CSV 进、文案与控件出，图标也一并给出）是 ``minihub_device_widget`` ；
    ``when`` / ``if`` / ``then`` 的求值在 ``minihub_rules`` ，一次调用召回一个场景的在
    ``minihub_scenes`` ；HTTP 路由、把事件总线镜像到 ``/ws/events`` 、分发控制台与地图
    瓦片在 ``minihub_web`` ；设备侧 MCP 工具（需要 ``components/esp-claw`` ）在
    ``minihub_mcp_server`` ；把产品级参数覆盖应用到 Zigbee 与 BLE Mesh 的 props 上的，
    是 ``minihub_product_params`` 。

商店与模块
    商店是为了动态安装插件，便于更灵活的扩展。目录刷新与安装、升级、卸载在
    ``minihub_store`` ；读清单、校验 ABI、加载在 ``minihub_module`` ；JSON 包合并进适配
    器视图在 ``minihub_pack`` ；QuickJS 工作线程与 ``host`` 对象在 ``minihub_js`` 。

射频
    Zigbee 、 BLE Mesh 、 Matter 、 Thread 的栈胶水各占一个目录 —— ``minihub_zigbee`` 、
    ``minihub_ble_mesh`` 、 ``minihub_matter`` 、 ``minihub_thread`` ——
    给模块用的通用 NimBLE GATT 客户端在 ``minihub_ble_gatt`` 。

    射频/设备适配器在 ``components/minihub_protocol_adapters/`` 下：
    ``adapter_zigbee`` 、 ``adapter_ble_mesh`` 、 ``adapter_matter`` 、
    ``adapter_thread`` 、 ``adapter_modbus`` 。登记设备点的有四个 —— Zigbee 、
    BLE Mesh 、 Matter 、 Modbus ； Thread 只做 OpenThread Border Router，不登记
    设备点（见 :doc:`radios` ）。网络上行（Wi-Fi STA / SoftAP / 以太网）在
    ``minihub_net`` （含薄层 ``wifi`` adapter 登记）。承载 ``host.mqtt`` 的 MQTT
    loopback 客户端与板载 broker 同在 ``minihub_mqtt_broker`` 。各射频适配器仍是
    独立组件，产品要去掉哪个就去掉哪个。

可选硬件
    只有板子要它们时才编进来：带屏板的本机 UI 是 ``minihub_lcd``
    （ ``CONFIG_MINIHUB_LCD_UI`` ，某些板上与 Ethernet 互斥）；RTSP 拉流与录制是
    ``minihub_rtsp_cam`` ；录下来的东西要有地方落， ``minihub_usb_msc`` 把 USB U 盘挂在
    ``/usb0`` 。

.. only:: not hide_l3

   ``minihub_module`` 也会加载原生模块，
   ``components/minihub_module/host_elf_symbols_generated.c`` 就是它们链接时用的
   Host 符号表。

``tools/`` 按你在做什么
-----------------------

构建与烧录
    ``build_firmware.py`` 是正门（ ``-app APP build``、 ``app-flash``、
    ``monitor``、 ``modules`` ）。 ``sync_host_manifest.py`` 把产品的
    ``manifest.yml`` 变成 IDF 构建读取的生成文件， ``compose_sdkconfig.py``
    对 sdkconfig 片段做同样的事。 ``gen_flash_binaries.py`` 产出量产烧录套件，
    ``publish_firmware.py`` 把一个版本写进 ``Firmwares/``。

商店模块
    ``new_store_module.py`` 按模板生成一层脚手架。
    ``pack_store_module.py`` 构建 ``.mhub``。 ``sync_store_catalog.py`` 写出
    对应的 ``catalog.json``。 ``store_server.py`` 把部署根以 HTTP 提供出去，
    Hub 从那里拉包。 ``build_store_modules.py`` 编译并打包 Host 构建发现的模块。

.. only:: not hide_l3

   ``tools/build_plugin_standalone.py`` 在 Host 树外交叉编译原生模块，
   ``tools/check_plugin_sdk.py`` 与 ``tools/check_module_so_layout.py`` 校验它
   只链接了导出的 Host 符号。

文档
    ``build_docs.sh`` 构建本指南（en、zh_CN 或全部），有告警就失败。
    ``preview_docs.py`` 把构建出的 HTML 提供服务。
    ``gen_deploy_index.py`` 写资源清单。

插件作者也可单独依赖 ``docs/module_sdk/``：其中含模块可 include 的头文件、符号
白名单，以及打包脚本副本，足以支撑树外编写。``python3 tools/minihub_plugin_mcp.py``
向 IDE 提供同一套脚手架与校验。

生成物，不要手改
----------------

下面这些由构建写出，你改的内容会在下一次构建时被覆盖。

.. list-table::
   :header-rows: 1
   :widths: 52 48

   * - 路径
     - 由谁写入
   * - ``application/APP/main/idf_component.yml``
     - ``tools/sync_host_manifest.py``
   * - ``application/APP/main/adapters_setup.generated.c``
     - ``tools/sync_host_manifest.py``
   * - ``cmake/generated/minihub_components.cmake``、
       ``cmake/generated/sdkconfig_stacks.gen``
     - ``tools/sync_host_manifest.py``、 ``tools/compose_sdkconfig.py``
   * - ``components/minihub_device_widget`` 内嵌 CSV 与注册表
     - ``components/minihub_device_widget/tools/gen_device_widget.py``
   * - ``components/minihub_module/include/minihub_abi_symbols.gen.h``
     - ``tools/gen_abi_contract.py``
   * - ``docs/_static/architecture/``
     - 文档构建时的 ``docs/_extensions/stage_architecture.py``
   * - 设备上的 ``/data/config/devices.csv``
     - 固件用内嵌目录重新播种

设备上的路径
------------

运行中的 Hub 写下的东西不在仓库里。 ``/data`` 是 ``storage`` 分区， ``/store``
是 ``modstore``；布局以及哪些文件在首次启动时从出厂 JSON 复制过去，写在
:doc:`architecture`。

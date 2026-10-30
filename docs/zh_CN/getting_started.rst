快速开始
========

:link_to_translation:`en:[English]`

环境要求
--------

* ESP-IDF 6.2（支持 ESP32-S31），并使用下文记录的固定修订
* Python 与编译工具链 — 按 ESP-IDF 安装指南配置：
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html#installation
* PyYAML（``pip install pyyaml``）— 构建与商店打包辅助脚本需要
* Matter（非必需，使能 ``CONFIG_MINIHUB_MATTER_ADAPTER`` 时需要）：
  ``ESP_MATTER_PATH`` 指向本地 esp-matter 仓库

.. _minihub-toolchain-pin-zh:

固定工具链
----------

本仓库不带 ESP-IDF（既非子模块也无 gitlink），所以这里记录开发时实际使用的修订。
用别的修订编译，ESP32-S31 上的失败很可能只是工具链不匹配。

.. list-table::
   :header-rows: 1
   :widths: 22 48 30

   * - 组件
     - 当前使用的修订
     - 为什么重要
   * - ESP-IDF
     - ``master`` 分支 ``434d5fd100b90a03c19018c572809cc29c7db28b``，
       ``git describe`` 为 ``qa-test-full-v6.2-20261016-70-g434d5fd100b``
       （即 ESP-IDF 6.2.0 之后再 70 个提交）
     - ESP32-S31 的支持比任何 6.2 release tag 都新；IDF 5.x 完全没有 S31 工具链
   * - Target
     - ``esp32s31``，且每次都要 ``--preview``
     - 该 target 仍是 preview 状态，不加参数 ``idf.py`` 会直接拒绝

要把自己的环境固定到同一个 ESP-IDF：

.. code-block:: bash

   git clone -b master https://github.com/espressif/esp-idf.git
   cd esp-idf && git checkout 434d5fd100b90a03c19018c572809cc29c7db28b
   ./install.sh esp32s31 && . ./export.sh

``tools/build_firmware.py`` 读取 ``$IDF_PATH``，未设置时使用仓库同级的
``esp-idf/`` 目录（已被 git 忽略）。放在别处就必须
``export IDF_PATH=...``，否则构建会用到的并不是上表记录的那一份。

克隆与首次编译
--------------

在仓库根目录：

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build
   ./tools/build_firmware.py -app minihub -p /dev/ttyUSB0 app-flash monitor

``build_firmware.py`` 封装 ``idf.py``：合成 sdkconfig、同步 manifest、应用板级
默认值，并在 ``application/minihub/`` 下调用 IDF 构建。
如果 ``-app`` 为其它产品（例如 ``my_app`` ），则在 ``application/my_app/`` 下调用
IDF 构建。

构建产物落在 ``application/minihub/build/`` （标准 IDF 输出目录）：可烧录的固件镜像是
``application/minihub/build/minihub.bin``，同目录还有 ``bootloader/``、
``partition_table/`` 与 ``flasher_args.json``。

默认产品（minihub）
-------------------

通用 Host 位于 ``application/minihub/``，默认 sdkconfig 组合见
``configs/sdkconfig/app.yml``：以太网优先（Wi-Fi STA 回退）、USB MSC、RTSP、Modbus、
商店动态模块等。参考板：``esp32_s31_function_coreboard_1``。

烧录后预期启动顺序
------------------

典型串口日志（可按原文检索）：

1. ``MiniHub starting...``
2. ``storage mounted at /data`` （随后 ``modstore`` 挂到 ``/store`` ）
3. ``uplinks prepared: wifi=… eth=… (wifi STA deferred)`` — 以太网已起，STA 尚未连接
4. ``HTTP server on port 80`` — Web 在射频栈之前就会起来
5. 射频适配器 start（Zigbee / BLE Mesh 等，视编译选项）
6. ``eth got IP`` 或 ``wifi got IP`` （后者在 ``minihub_net_start_wifi()`` 之后）
7. ``MiniHub ready (net=… prov=… web_ui=… lcd_ui=… eth=… wifi=…)``

``MiniHub ready`` 表示功能已准备，不代表已经拿到 IP。

.. image:: ../_static/boot/boot_timeline.zh_CN.svg
   :align: center
   :alt: MiniHub 上电时序：固件 → 存储 → 上行 → Web → 适配器 → 地址 → 就绪

本指南 URL 里的 ``HUB`` 就是 ``eth got IP`` 或 ``wifi got IP`` 打印的地址
（例如 ``192.168.1.50`` ）；指南不注册主机名。

浏览器打开 ``http://HUB/`` （可能跳到 ``/login`` ）。首次访问用 ``admin`` 登录并设置
至少 6 位密码，之后在 Web UI 中验证设备、操作设备。换皮 HTML 见 :doc:`web_console`。

``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认）开启时，Web UI 会要求登录；REST 客户端需带
会话 Cookie 或 ``Authorization: Bearer``。可选 token：
``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` （非空时以它为准，只有它为空才读 NVS
``api_token`` ）。见 :doc:`faq`。

网络日志见 :doc:`networking` (``eth got IP``、``wifi got IP``)。

产品模型
--------

.. code-block:: text

   application/APP/          产品（板级、manifest、出厂配置）
   components/               共享 Core + 适配器
   configs/                   共享 manifest 基座 + sdkconfig 片段
   cmake/generated/          sync 输出（git 忽略）

* 选择产品：``-app APP`` 或 ``MINIHUB_APP=APP`` （默认 ``minihub``）。
  详见 :doc:`application/index`。
* Manifest：``application/APP/manifest.yml`` （通常 ``extends: _host_base`` ）
* Sdkconfig：``application/APP/configs/sdkconfig/app.yml``

修改 manifest 后直接 ``./tools/build_firmware.py -app APP build`` 即可（编译前会自动合并）。

合成 sdkconfig：``tools/compose_sdkconfig.py``。协议栈列表：
``cmake/generated/sdkconfig_stacks.gen``。

逐目录说明（含构建生成的文件）见 :doc:`source_tree`。

商店插件快速路径
------------------

1. 新增 ``examples/plugins/ID/`` （或通过 ``MINIHUB_PLUGIN_DIRS``
   指向你自己的仓库；见 :doc:`plugins/develop_l1` 或
   :doc:`plugins/develop_l2` ）
2. ``python3 tools/pack_store_module.py pack ID``
3. 本机运行 ``python3 tools/store_server.py`` （Hub 通过局域网拉包，不是在设备上跑）
4. 打开 Web UI（ ``http://HUB/`` ）并进入 **商店**
5. 在商店中将 自定义商店 URL 设为该服务，刷新目录后在商店 UI 中
   安装 ``ID`` （L1 用 ``pack`` 作为类型）

.. only:: not hide_l3

   L3 原生模块见 :doc:`plugins/develop_l3`。打包前先编译 ``.so``。

新类型 / 控件：改 Host ``model/*.csv`` 后重编，或在已运行 Hub 上用 REST 上传两份
CSV （``PUT /api/dev/catalog/…``；无 Web 页面—— :doc:`type_catalog`）。演示：L1
``examples/plugins/ble_mesh_pack``，L2 ``color_bulb_js`` / ``meter_plug_js``。

.. only:: not hide_l3

   仓库内不再有 L3 演示，用
   ``tools/new_store_module.py --tier l3 --id my_native`` 生成。

OEM Host 产品见 :doc:`host_customization`。

Matter 编译
--------------

使能 ``CONFIG_MINIHUB_MATTER_ADAPTER``，并导出 ``ESP_MATTER_PATH`` 后编译：

.. code-block:: bash

   export ESP_MATTER_PATH=/path/to/esp-matter
   ./tools/build_firmware.py -app minihub build

Matter 控制器网络与 REST API 见 :doc:`matter`。

延伸阅读
--------

* **用 AI 开发** — :doc:`vibe_coding`
* **理解 Hub** — :doc:`architecture`，再 :doc:`data_flow`，再
  :doc:`type_catalog` 与 :doc:`data_model`
* **网络** — :doc:`networking`
* **射频与接入** — :doc:`radios` （Matter / Thread BR / Zigbee / BLE Mesh）
* **Hub 固件升级** — :doc:`ota`
* **先看** ``application/`` **再做 OEM Host** — :doc:`application/index` 再
  :doc:`host_customization` 再 :doc:`production`
* **替换 Web 控制台** — :doc:`web_console`
* **写插件** — :doc:`plugins/overview`
* **调 API** — :doc:`api/host_rest` · :doc:`api/mcp` · :doc:`api/index`

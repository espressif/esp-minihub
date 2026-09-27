快速开始
========

:link_to_translation:`en:[English]`

环境要求
--------

* ESP-IDF ~6.2（支持 ESP32-S31）
* Python 3.10+ 与 PyYAML（``pip install pyyaml``）
* Matter（非必需，使能 ``CONFIG_MINIHUB_MATTER_ADAPTER`` 时需要）：
  ``ESP_MATTER_PATH`` 指向本地 esp-matter 仓库

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

默认产品（minihub）
-------------------

通用 Host 位于 ``application/minihub/``，默认 sdkconfig 组合见
``configs/sdkconfig/app.yml``：以太网优先（Wi-Fi STA 回退）、USB MSC、RTSP、Modbus、
Store 动态模块等。参考板：``esp32_s31_function_coreboard_1``。

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

浏览器打开 ``http://HUB/`` （可能跳到 ``/login`` ）。REST 基址：
``http://HUB/api/`` 。首次访问：用户名 ``admin`` ，设置至少 6 位密码。换皮 HTML
见 :doc:`web_console` 。

首次检查（把 ``HUB`` 换成 ``eth got IP`` 或 ``wifi got IP`` 中的地址）：

.. code-block:: bash

   curl -s http://HUB/api/
   curl -s http://HUB/api/system
   curl -s http://HUB/api/points

打开 ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认）时，未带会话 Cookie 或
``Authorization: Bearer`` 会返回 HTTP 401。可选 token：
``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` 或 NVS ``api_token`` 。见 :doc:`faq` 。

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

Store 插件快速路径
------------------

1. 新增 ``examples/plugins/ID/`` （或通过 ``MINIHUB_PLUGIN_DIRS``
   指向厂商仓库；见 :doc:`plugins/develop_l1` 、
   :doc:`plugins/develop_l2` 或 :doc:`plugins/develop_l3` ）
2. ``python3 tools/pack_store_module.py pack ID``
3. 本机运行 ``python3 tools/store_server.py`` （Hub 通过局域网拉包，不是在设备上跑）
4. Hub：``POST /api/store/config``，``custom_url`` 指向该服务
5. ``POST /api/store/refresh``，再 ``POST /api/store/install``，body 为
   ``{"kind":"module","id":"ID"}`` （L1 用 ``"kind":"pack"``；或 Web UI → Store）

新类型 / 控件：改 Host ``model/*.csv`` 后重编，或在已运行 Hub 上
``PUT /api/dev/catalog/…`` （:doc:`type_catalog`）。演示：L1
``examples/plugins/ble_mesh_pack``，L2 ``color_bulb_js``，L3
``color_bulb`` / ``meter_plug``。

OEM Host 产品见 :doc:`host_customization`。

Matter 编译
--------------

使能 ``CONFIG_MINIHUB_MATTER_ADAPTER`` ，并导出 ``ESP_MATTER_PATH`` 后编译：

.. code-block:: bash

   export ESP_MATTER_PATH=/path/to/esp-matter
   ./tools/build_firmware.py -app minihub build

Matter 控制器网络与 REST API 见 :doc:`matter`。

延伸阅读
--------

* **用 AI 开发** — :doc:`vibe_coding`
* **理解 Hub** — :doc:`architecture`，再 :doc:`architecture_overview`，再
  :doc:`type_catalog` 与 :doc:`data_model`
* **网络** — :doc:`networking`
* **射频与接入** — :doc:`radios` （Matter / Thread BR / Zigbee / BLE Mesh）
* **Hub 固件升级** — :doc:`ota`
* **先看** ``application/`` **再做 OEM Host** — :doc:`application/index` 再
  :doc:`host_customization` 再 :doc:`production`
* **替换 Web 控制台** — :doc:`web_console`
* **写插件** — :doc:`plugins/overview`
* **调 API** — :doc:`api/host_rest` · :doc:`api/mcp` · :doc:`api/index`

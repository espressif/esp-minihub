插件概览
========

:link_to_translation:`en:[English]`

Host 与插件
-----------

* **Host（静态链接）：** Core、Web、MCP；以及 Kconfig 打开的 Zigbee / BLE Mesh /
  Matter / MQTT / Modbus
* **Store** （ ``.mhub`` ）：生态 / 厂商适配器，分 L1、L2、L3（选型见下文）

不要把厂商云凭证写进通用 Host Core ``config/``。令牌应放在 ``/store/modules/ID/``
或模块私有 NVS。

模块分层
--------

先按能力和约束选定一层，再打开该层实现章。打包、目录与安装见
:doc:`pack_and_catalog` 。

.. list-table::
   :header-rows: 1
   :widths: 12 30 38 20

   * - 层级
     - 包内
     - 适用 / 约束
     - 实现
   * - L1
     - ``runtime: pack`` 加 ``inject`` ；无 ``main.js`` 、无 ``.so`` 。
       运行时合并到 ``adapters.json`` 。
     - **适用：** 已有 Host 适配器的默认参数（BLE Mesh bind、UUID 过滤、
       Modbus 映射）。**不要** inject 射频 ``devices[]`` （设备由入网登记
       一条点）。不要写进 Host Core ``config/`` 。
       **不适用：** 自定义逻辑、云端 API、新适配器、厂商认领 / OTA。
     - :doc:`develop_l1`
   * - L2
     - ``runtime: js`` 加 ``main.js`` ；可选 ``ui.json`` / ``web/`` 。
     - **适用：** 轻量自动化、HTTP 轮询、REST 与点位胶水、无原生 Web 面板；
       也可作 L3 前原型。
       **要求：** ``CONFIG_MINIHUB_DYNAMIC_MODULES`` （ ``app.yml`` 里
       ``dynamic_modules`` ）。
     - :doc:`develop_l2`
   * - L3
     - 省略 ``runtime`` 或写 ``native`` ，加 MHMD 封装 ``.so`` ；可选
       ``ui.json`` / ``web/`` 。Host ``dlopen`` 后调用
       ``minihub_module_entry`` 。
     - **适用：** 新协议 / 厂商适配器、C 性能或 Host ABI。
       **要求：** 与 Host 同架构交叉编译（或跟固件一起编）；
       ``abi_version`` / ``core_api_*`` 覆盖当前 Host；符号在 allowlist
       内。见 :doc:`../api/module_abi` 。
       参考 ``miot`` ： ``module.json`` + ``miot.so`` + ``ui.json`` ，无
       ``main.js`` 、无 ``inject`` ，是 L3 不是 L1。
     - :doc:`develop_l3`
   * - Hybrid
     - **仅 L2 + L3：** 同一 ``.mhub`` 内同时有 ``.so`` 与 ``main.js`` ，
       ``runtime: hybrid`` ，且 ``entry`` 为 ``native`` 或 ``js`` （二选一）。
     - **适用：** 同一模块既要原生适配器又要 JS。
       **不是** L1 / L2 / L3 的任意组合，也 **不能** 把 L1 ``inject``
       打进同一个 Hybrid 包（ ``runtime: pack`` 与 ``hybrid`` 互斥）。
       需要配置包时另装一个 L1 模块。也不存在「三者同包」的 runtime。
     - :doc:`develop_l3` 与
       :doc:`develop_l2`

``module.json`` 各层都有； ``ui.json`` 是 Web 表单，L2 / L3 / Hybrid 都可带。
这 **不是** L1。L1 只指 ``inject`` 经 ``minihub_pack`` 合并进已有适配器块
（例如给 BLE Mesh 下发 bind）。安装 L3 时 Host 最多在 ``adapters.json`` 里
建一个空槽（ ``enabled`` + ``devices`` ）；业务 JSON 由模块自己读写。

**设备模型：** 一台物理设备登记一条点（``协议.设备``），实时值放
``props.params``。``type`` 字符串与 ``devices.json`` 须落在 Host 类型目录里，
设备页才有标准控件（:doc:`../type_catalog`）。点位字段与 adapters / 规则见
:doc:`../data_model`。不要按灯/开关发明 C 结构。演示：L1
``examples/plugins/ble_mesh_pack``，L2 ``color_bulb_js``，L3
``color_bulb`` / ``meter_plug``。

源码与设备路径
--------------

可选仓库内源码：

* 生产：``components/minihub_plugins/ID/`` （参考 ``miot``）
* 演示：``examples/plugins/ID/`` ，通过 ``MINIHUB_PLUGIN_DIRS`` （须在
  **CMake 之前** export）。索引：``examples/README_CN.md``。公共 Host
  默认 ``store_preinstall`` 为空。

安装后在设备上：

.. code-block:: text

   /store/modules/ID/
     module.json      # 各层清单（不是 L1）
     main.js          # 仅 L2 / Hybrid
     ID.so            # L3 / Hybrid（文件名见 module.json 的 binary）
     ui.json          # 可选 Web 表单（L2 / L3 / Hybrid）
     .load_guard      # Host 加载期间写入

开发机部署目录（``MINIHUB_STORE_DEPLOY``，默认 ``~/workspace/minihub_store`` ）：

.. code-block:: text

   DEPLOY/catalog.json
   DEPLOY/ID/ID.mhub

.. _store-preinstall:

工厂预装
--------

在 ``application/APP/manifest.yml`` 中：

.. code-block:: yaml

   store_preinstall:
     - miot

固件打包会把 ``.mhub`` 写入 ``modstore`` （挂载为 ``/store`` ）。
``module.json`` 里的 ``preinstall`` 字段不参与工厂打包。

开发循环（摘要）
----------------

1. 编辑 ``components/minihub_plugins/ID/`` （或设了
   ``MINIHUB_PLUGIN_DIRS`` 的 ``examples/plugins/ID/``）
2. L3 先编 ``.so`` （仓库外单独交叉编译，或跟 Host 一起编，见
   :doc:`develop_l3` ），再 ``python3 tools/pack_store_module.py pack ID``
3. ``.mhub`` 已在 store 目录后：
   ``python3 tools/sync_store_catalog.py --from-store`` ，再
   ``python3 tools/store_server.py``
4. Hub：从目录安装或上传 ``.mhub``，再 ``POST /api/hub/reload``

选定层级后的脚手架见上文「模块分层」各实现章；目录与发布见
:doc:`pack_and_catalog` 。

``module.json`` 字段
--------------------

各层都需要：

* ``id`` — 目录名与 Store REST ``id``
* ``name`` · ``version`` — 目录 / ``GET /api/modules`` 展示
* ``enabled`` — 安装后默认启用

层级 / 加载器：

* ``runtime`` — ``pack`` (L1)、``js`` (L2)、省略/原生 (L3)、或 ``hybrid``
  （仅 L2 + L3 同包，见上表）
* ``adapter`` — Host 会创建/启用的 ``adapters.json`` 键
* ``binary`` — L3 解压后的文件名 (如 ``miot.so``)
* ``ui_file`` — 可选 Web 表单 (``ui.json``)
* ``abi_version`` · ``core_api_min`` · ``core_api_max`` — 须覆盖 Host 版本
  (见 :doc:`../api/module_abi`)
* ``capabilities`` — 说明性标签 (``points``、``auth``、``device_sync``)
* ``vendor`` · ``description`` — 目录文案
* ``inject`` — 仅 L1 合并载荷；Hybrid / L2 / L3 包不走 ``minihub_pack``
* ``preinstall`` — 不参与工厂打包；请用 :ref:`store-preinstall` 中的
  ``store_preinstall``

类型目录与 ``devices.json``：:doc:`../type_catalog`。
adapters/规则/场景 JSON：:doc:`../data_model`。
工厂镜像：:doc:`../production`。

ABI 契约
--------

L3 须遵守 ``docs/module_sdk/host_export_allowlist.json`` 与
``minihub_module_abi.h``。见 :doc:`../api/module_abi`。

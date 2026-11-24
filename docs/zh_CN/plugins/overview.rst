插件概览
========

:link_to_translation:`en:[English]`

Host 与插件
-----------

随每台设备出货的代码链进固件：Core，以及 Kconfig 打开的射频栈与适配器
（Zigbee、BLE Mesh、Matter、MQTT、Modbus）。Web 控制台是链进去的 HTTP 服务，
加上放在 ``storage`` 分区里的 HTML、CSS、JavaScript，所以换皮不用重编 app
（见 :doc:`../web_console`）。MCP 服务是独立组件，默认打开
（``CONFIG_MINIHUB_MCP_SERVER``）。

开发者 / 生态适配器不同。它们常带不能出现在公开构建里的凭证，也不是每个产品都需要。
这些以后台 ``.mhub`` 包从商店安装，分 JSON、JS（选型见下文）。

.. only:: not hide_l3

   ELF 是原生（``.mhub`` 内的 ``.so`` ）层。Hybrid 是同一包里的 JS 加 ELF。

不要把厂商云凭证写进通用 Host Core ``configs/``。写在那里的内容会烧进公开构建的
每一台设备。令牌放在 ``/store/modules/ID/`` 或模块私有 NVS。

模块分层
--------

先按能力和约束选定一层，再打开该层实现章。打包、目录与安装各层共用，见
:doc:`pack_and_catalog`；从空目录做到设备上可运行，见 :doc:`first_plugin`。
每一层在 ``.agent/skills/`` 下还有一个给 AI 助手用的项目 skill，按层级命名，清单见
:doc:`../vibe_coding`。

按你会写什么来选：

* **JSON** —— 只有配置，完全不写代码：用 JSON 包给 Host 已有的适配器补默认参数。
* **JS** —— 在嵌入式 **QuickJS-NG** 引擎中运行 JavaScript。适合熟悉 JS 的开发者：
  不需要交叉编译、不产出 ``.so``，单个模块出错也不会带走 Host。但它不是浏览器，
  动手前先读 :doc:`develop_js` 里的运行环境。

.. only:: not hide_l3

   * **ELF** —— 原生 C/C++ 编成 ``.so``，经 Host ABI vtable 加载。适合需要导出
     接口、线程或二进制协议的嵌入式开发者。

.. list-table::
   :header-rows: 1
   :widths: 12 30 38 20

   * - 层级
     - 包内
     - 适用 / 约束
     - 实现
   * - JSON
     - ``runtime: pack`` 加 ``inject``；无 ``main.js``、无 ``.so``。
       运行时合并到 ``adapters.json``。
     - **适用：** 已有 Host 适配器的默认参数（BLE Mesh bind、UUID 过滤、
       Modbus 映射）。**不要** inject 射频 ``devices[]`` （设备由入网登记
       一条点）。不要把你自己的默认值写进 Host Core ``configs/``。
       **不适用：** 自定义逻辑、云端 API、新适配器、插件关联 / OTA。
     - :doc:`develop_json`
   * - JS
     - ``runtime: js``： ``main.js`` 由嵌入式 **QuickJS** 引擎执行；
       可选 ``meta.json`` / ``web/``。
     - **适用：** 自动化、HTTP 轮询、REST 与点位胶水、Web 面板 —— 用 JavaScript
       写，没有原生代码，也不需要交叉编译。
       **要求：** ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 与 ``CONFIG_MINIHUB_JS_PLUGINS``
       （ ``app.yml`` 里 ``dynamic_modules`` ）。
       引擎单线程：所有 JS 模块共用一个回调队列，长任务必须让出
       （ ``host.adapter.poll`` / ``setTimeout`` ）。
     - :doc:`develop_js`

.. only:: not hide_l3

   .. list-table::
      :header-rows: 1
      :widths: 12 30 38 20

      * - 层级
        - 包内
        - 适用 / 约束
        - 实现
      * - ELF
        - 省略 ``runtime`` 或写 ``native``，加 MHMD 封装 ``.so``；可选
          ``meta.json`` / ``web/``。Host ``dlopen`` 后调用
          ``minihub_module_entry``。
        - **适用：** 新协议 / 厂商适配器、C 性能或 Host ABI。JS 也可先做原型。
          **要求：** 与 Host 同架构交叉编译（或跟固件一起编）；
          ``abi_version`` / ``core_api_*`` 覆盖当前 Host；符号在 allowlist
          内。见 :doc:`../api/module_abi`。
          参考 ``my_native``： ``module.json`` + ``my_native.so`` （+ 可选
          ``devices.json``），无 ``main.js``、无 ``inject``，是 ELF 不是 JSON。
        - :doc:`develop_elf`
      * - Hybrid
        - **仅 JS + ELF：** 同一 ``.mhub`` 内同时有 ``.so`` 与 ``main.js``，
          ``runtime: hybrid``，且 ``entry`` 为 ``native`` 或 ``JS`` （二选一）。
        - **适用：** 同一模块既要原生适配器又要 JS。
          **不是** JSON / JS / ELF 的任意组合，也 **不能** 把 JSON ``inject``
          打进同一个 Hybrid 包（ ``runtime: pack`` 与 ``hybrid`` 互斥）。
          需要配置包时另装一个 JSON 模块。也不存在「三者同包」的 runtime。
        - :doc:`develop_elf` 与
          :doc:`develop_js`

各层都有 ``module.json``；同目录还可选 ``meta.json`` （``ui`` + ``capabilities`` ）。
有 ``module.json`` 并不等于 JSON。JSON 的含义是： ``inject`` 经 ``minihub_pack`` 合并进
已有适配器块，最常见的例子是给 BLE Mesh 下发 bind。

.. only:: not hide_l3

   ``meta.json`` 在 ELF / Hybrid 上同样可用。安装 ELF 时 Host 最多在
   ``adapters.json`` 里建一个空槽（ ``enabled`` + ``devices`` ）；业务 JSON
   由模块自己读写。

各层共用同一套设备模型。一台物理设备登记一条点（``协议.设备``），实时值放
``props.params``。``type`` 字符串与 ``devices.json`` 须落在 Host 类型目录里，概览页
才有标准控件（:doc:`../type_catalog`）。点位字段与 adapters / 规则见
:doc:`../data_model`。不要为灯 / 开关单独发明 C 结构 —— 类型就是一个名字加一组
参数，新设备品类因此不必改固件。演示：JSON ``examples/plugins/ble_mesh_pack``，JS
``color_bulb_js``。额外第三方插件不在本仓库；在 CMake 之前把 ``MINIHUB_PLUGIN_DIRS``
指到旁路商店插件仓库。

.. only:: not hide_l3

   仓库内不再有 ELF 演示，用
   ``tools/new_store_module.py --tier l3 --id my_native`` 生成。

源码与设备路径
--------------

仓库内源码是可选的。插件也可以完全放在旁路仓库里开发：

* 演示：``examples/plugins/ID/``
  （额外树须在 **CMake 之前** export ``MINIHUB_PLUGIN_DIRS``）。索引：
  ``examples/README_CN.md``。公共 Host 默认 ``store_preinstall`` 为空。

安装后在设备上：

.. code-block:: text

   /store/modules/ID/
     module.json      # 各层清单（不是 JSON）
     main.js          # JS
     meta.json         # 可选 ui + capabilities
     .load_guard      # Host 加载期间写入

.. only:: not hide_l3

   .. code-block:: text

      ID.so            # ELF / Hybrid（文件名见 module.json 的 binary）
      meta.json         # ELF / Hybrid 也可带

开发机部署目录（``MINIHUB_PLUGINS_DEPLOY_DIR``，默认 ``~/workspace/minihub_store`` ）：

.. code-block:: text

   DEPLOY/Stores/Common/ID/catalog.json
   DEPLOY/Stores/Common/ID/ID.mhub

JSON 与 JS 使用该布局。原生与 hybrid 模块不发布：打包器不给它们分配目录，也不删除
任何东西。早期构建留下的目录，只有在启用 ``move_packages`` 的 catalog 同步中才会被
清理，打包器本身并不会删。

.. _store-preinstall:

工厂预装
--------

工厂是否捆绑插件由产品决定，不由模块决定。把模块 id 写在
``application/APP/manifest.yml`` 中：

.. code-block:: yaml

   store_preinstall:
     - color_bulb_js

.. only:: not hide_l3

   ELF 的 id（例如 ``my_native`` ）同样写在这里。

固件打包把 ``.mhub`` 写入 ``modstore`` （挂载为 ``/store`` ）。
``module.json`` 里的 ``preinstall`` 字段不参与工厂捆绑。

开发者脚本
----------

.. list-table::
   :header-rows: 1
   :widths: 46 54

   * - 脚本
     - 作用
   * - ``tools/new_store_module.py --tier l1|l2 --id ID``
     - 在 ``examples/plugins/`` 下生成脚手架
   * - ``tools/pack_store_module.py pack ID|all``
     - 打 ``.mhub`` zip（不编译 C）
   * - ``tools/sync_store_catalog.py``
     - 在每个已部署 ``.mhub`` 旁重写 ``catalog.json`` （默认）
   * - ``tools/sync_store_catalog.py all``
     - 先从源码打包，再同样重写 ``catalog.json``
   * - ``tools/store_server.py``
     - 本地目录 HTTP 服务
   * - ``tools/minihub_plugin_mcp.py`` （IDE MCP）
     - 脚手架 / 校验 / 打包；不交叉编译 ``.so``

各层级细节：:doc:`develop_json`、:doc:`develop_js`。

.. only:: not hide_l3

   ELF 另有 ``tools/new_store_module.py --tier l3``、
   ``tools/pack_store_module.py mhmd`` （给 ELF 套 MHMD 头）、
   ``tools/build_firmware.py modules``、
   ``tools/build_plugin_standalone.py ID`` （无 IDF 的 CMake 编译）、
   ``tools/generate_devices_property_from_json.py`` （ ``devices.json`` →
   ``device_desc.gen.h``，可选）与 ``tools/check_plugin_sdk.py check`` （另有
   ``coverage`` 与 ``emit-sizes`` ；开了 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 时
   Host 的 CMake 会自己跑 ``check``）。见 :doc:`develop_elf`。

``module.json`` 字段
--------------------

各层都带 ``module.json``。加载器真正强制的只有两个字段：

* ``id`` — 目录名与商店 REST ``id``；安装时还必须与 catalog 条目的 ``id`` 一致
* ``adapter`` — Host 要创建并启用的 ``adapters.json`` 键。**每一层** （含 JS）
  缺失或为空都通不过校验，所以没写 ``adapter`` 的 JS 插件根本不会加载

**``requires``** 是商店侧的能力门禁：取自 ``Zigbee`` 、 ``BLE_mesh`` 、
``Thread_Matter`` 、 ``Matter`` 、 ``MQTT_Broker`` 、 ``Modbus`` 的字符串列表。
Hub 在 ``GET /api/catalog`` 上把本机已启用的能力写成 ``caps=`` ；商店会拿掉
``requires`` 未被覆盖、且尚未安装的插件。 ``[]`` 或省略表示不设门禁。
它与 ``meta.json`` 里的 ``capabilities`` （ ``points`` 、 ``auth`` 、
``device_sync`` ）不是一回事：后者只是说明性标签，不参与目录过滤。

其余字段都有默认值，不写不算错：

* ``name`` · ``version`` — 目录 / ``GET /api/modules`` 展示；``version`` 缺省
  ``0.0.0``
* ``enabled`` — 缺省 true
* ``abi_version`` · ``core_api_min`` — 缺省取 Host 自己的值；``core_api_max``
  缺省 0，表示不设上界
* ``runtime`` — ``pack`` （JSON）或 ``JS`` （JS）
* ``requires`` — 省略时按 ``[]`` ；打包时原样写入 ``catalog.json``

``ui`` / ``capabilities`` 优先读同目录的 ``meta.json``，没有该文件时才回落到
``module.json`` 里的同名字段；``ui`` 是可选 Web 表单，``capabilities`` 为说明性标签
（``points``、``auth``、``device_sync``）。

* ``vendor`` · ``description`` — 目录文案
* ``inject`` — 仅 JSON 合并载荷
* ``preinstall`` — 不参与工厂打包；请用 :ref:`store-preinstall` 中的
  ``store_preinstall``

.. only:: not hide_l3

   ELF / Hybrid 另有：

   * ``runtime`` — 省略或 ``native`` (ELF)，或 ``hybrid`` （仅 JS + ELF 同包）
   * ``binary`` — ELF 解压后的文件名 (如 ``my_native.so``)
   * ``abi_version`` · ``core_api_min`` · ``core_api_max`` — 须覆盖 Host 版本
     (见 :doc:`../api/module_abi`)
   * Hybrid / ELF 包的 ``inject`` 不走 ``minihub_pack``

类型目录与 ``devices.json``：:doc:`../type_catalog`。
adapters/规则/场景 JSON：:doc:`../data_model`。
工厂镜像：:doc:`../production`。

.. only:: not hide_l3

   ABI 契约
   --------

   ELF 须遵守 ``docs/module_sdk/host_export_allowlist.json`` 与
   ``minihub_module_abi.h``。见 :doc:`../api/module_abi`。

打包与目录
==========

:link_to_translation:`en:[English]`

一个商店模块从开发机的源目录，走到 Hub 的 ``/store/modules/<id>/``。

前置条件
--------

* JSON / JS 打包不需要编 Host 固件
* Hub 加载 JS 时，烧进去的固件也必须打开
  ``CONFIG_MINIHUB_DYNAMIC_MODULES``
* 脚本缺 PyYAML 时在仓库根目录 ``pip install pyyaml``
* 可选：``export MINIHUB_PLUGINS_DEPLOY_DIR=~/workspace/minihub_store``

.. only:: not hide_l3

   * ELF **仓库外** 交叉编译：只要有 MHMD ``.so`` （见 :doc:`develop_elf` ），不必编固件
   * ELF **仓库内** 跟固件一起编：产品须打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES``
     （ ``app.yml`` 的 ``dynamic_modules`` ），否则 CMake 不会生成
     ``libadapter_*.so``
   * Hub **加载** ELF 时，烧进去的固件也必须打开上述选项

打包命令
--------

.. code-block:: bash

   # 指定 id 可打任意层级。
   python3 tools/pack_store_module.py pack ID
   python3 tools/pack_store_module.py pack all

   # .mhub 已在 store 目录后，按 zip 重写 catalog.json（默认；不需要插件源码）
   python3 tools/sync_store_catalog.py
   python3 tools/sync_store_catalog.py --deploy ~/workspace/minihub_store

   # 按插件源码打包，再同样重写 catalog
   python3 tools/sync_store_catalog.py all
   python3 tools/store_server.py

.. only:: not hide_l3

   .. code-block:: bash

      # 编译全部仓库内原生插件并打包 .mhub
      ./tools/build_firmware.py -app minihub modules

      # 单个原生插件
      ./tools/build_firmware.py -app minihub module ID

      # 已有仓库内 .so 时只再打包（不编译 C）
      ./tools/build_firmware.py -app minihub pack

      # 手动 MHMD（通常 pack 已包含）
      python3 tools/pack_store_module.py mhmd in.so out.so

      # 无 IDF 编译 ELF 模块（仓库内无示例，先用脚手架生成）
      python3 tools/build_plugin_standalone.py my_native

   ``./tools/build_firmware.py … modules`` 编译同时含 ``module.json`` 与
   ``CMakeLists.txt`` 的全部原生插件 （``examples/plugins/*/`` 以及
   ``MINIHUB_PLUGIN_DIRS``），并把 MHMD ``.so`` 留在构建目录。这次编译
   不创建商店插件目录。Host 固件 ``build`` 不编译这些插件。
   ``./tools/build_firmware.py … pack ID`` 把已有的 ``libadapter_ID.so``
   封装成本地 MHMD ``.so``，不写入 ``Stores/Common/ID/``，也不写入
   ``Stores/<chip>/``。

``.mhub`` 已经放进 store 目录后（例如
``~/workspace/minihub_store/Stores/Common/ble_mesh_pack/ble_mesh_pack.mhub`` ），运行
``sync_store_catalog.py`` （ ``--from-store`` 是默认行为）。它遍历
``Stores/Common/ID/ID.mhub``，读取 zip 内的 ``module.json``，在该包旁边写出
``catalog.json``。``ui`` 与 ``capabilities`` 留在包内 ``meta.json``。不需要插件源码。
``GET /api/catalog`` 聚合这些对象；只拷贝 zip
不会更新目录。默认部署根是 ``MINIHUB_PLUGINS_DEPLOY_DIR`` 或
``~/workspace/minihub_store``。

.. only:: not hide_l3

   同一脚本也在 ``module_sdk.zip`` 里。

部署目录布局
------------

.. code-block:: text

   DEPLOY/Stores/Common/ID/catalog.json
   DEPLOY/Stores/Common/ID/ID.mhub

默认 ``store_server.py`` 监听 ``http://0.0.0.0:8080``
(可用 ``--port`` 覆盖)。

外层容器：**zip** (``MHUB_PACK_FORMAT``，默认 ``zip``)。Host 解压：
``components/minihub_store/hub_zip.c`` (目前仅 zip)。

.. only:: not hide_l3

   **MHMD** 是内层原生镜像魔数 (``0x4D484D44``)，不是外层 zip 名称。

``catalog.json`` 条目 (由 ``sync_store_catalog.py`` 生成)
--------------------------------------------------------------

.. code-block:: JSON

   {
     "type": "module",
     "id": "esphome",
     "name": "ESPHome MQTT discovery",
     "version": "0.1.0",
     "targets": ["any"],
     "requires": ["MQTT_Broker"],
     "url": "/files/Stores/Common/esphome/esphome.mhub",
     "adapter": "esphome",
     "size": 5618
   }

不依赖协议栈的模块也要显式写出空列表：

.. code-block:: JSON

   {
     "type": "module",
     "id": "color_bulb_js",
     "name": "Color Bulb (JS)",
     "version": "0.1.0",
     "targets": ["any"],
     "requires": [],
     "url": "/files/Stores/Common/color_bulb_js/color_bulb_js.mhub",
     "adapter": "color_bulb_js",
     "size": 3762
   }

* ``type`` — 生成的目录里恒为 ``module``。标记纯配置 JSON 的是 ``config_only: true``
  （安装 URL 以 ``.json`` 结尾也算）；打包器同时写 ``runtime: "pack"``，包类型判定器
  与加载器都读它，据此把包交给 ``minihub_pack``，而不是按可加载模块处理。
  手写 ``"type": "pack"`` 只在 Hub 报告已安装状态时被认，**装不了** ：目录查找只在
  ``config_only`` 条目上让 ``pack`` 与 ``module`` 互相兜底，而 URL 以 ``.mhub``
  结尾的条目不属于它（ ``minihub_store.c:1104-1127,1081-1101`` ）
* ``url`` — ``.mhub`` 的路径或绝对 HTTP URL
* ``targets`` — 小写芯片文件夹名，与固件目录使用同一套字符串
  （例如 ``esp32s31`` ），任意芯片写成 ``any``。从 ``module.json``
  抄入时具体芯片名转为小写， ``any`` 、 ``ALL`` 和 ``*`` 都写成 ``any``。
  省略该字段时，JS 模块打包写成 ``["any"]``，其它模块写成 ``["esp32s31"]``
* ``requires`` — **打包前在 ``module.json`` 里写好。** 这个插件需要的能力，取自
  ``Zigbee`` 、 ``BLE_mesh`` 、 ``Thread_Matter`` 、 ``Matter`` 、 ``MQTT_Broker`` 、
  ``Modbus`` 。 ``[]`` 表示不需要。每一项都必须已编进固件且处于启用状态。列出的任一项
  不具备时，Hub 商店不把尚未安装的插件放进可安装列表。该列表原样抄进 ``catalog.json`` ；
  省略时写成 ``[]`` 。不要和 ``meta.json`` 的 ``capabilities`` （仅说明）混用
* ``adapter`` — 可选；安装时 Host 会在 ``adapters.json`` 中建槽
* ``size`` — **外层 zip** 字节数（须与磁盘上 ``.mhub`` 一致；``pack`` 会回写）

* ``binary_size`` — 内层原生镜像的大小，不是 zip 的大小。只要打包器在应用构建目录里找得到这个
  id 编好的原生镜像，就会把它盖进来。所以 ``runtime: js`` 的条目旁出现一个非零
  ``binary_size`` ，意思是同 id 早前那次构建留下了产物，不是说它是原生模块
  （ ``sync_store_catalog.py:282-290`` ）。
* ``sha256`` — 打包器对每个包算一份写在这里，商店接口也照原样把它给出去，但安装路径
  从不校验它 ：校验的是 ``size`` 必须相等，摘要不看。摘要校验只存在于
  ``components/minihub_update/`` 的固件与资源记录里。

两个文件里的 ``type`` 含义不同：生成的 ``catalog.json`` 每一层都写 ``module``，
而 JSON 包内的 ``module.json`` 写的是 ``pack``。

在运行中的 Hub 上安装
---------------------

**本地商店服务：** Web UI → 商店 → 刷新目录 → 安装。

**Web UI** — 打开 ``http://HUB/`` 并进入 ``商店``，将自定义商店 URL 设为本地
服务，刷新目录后安装 ``color_bulb_js``。安装后 Hub 会自动重载模块。

JSON 包在商店 UI 中显示为 pack 条目。

验证安装
--------

.. note::

   ``$HUB`` 是设备的局域网地址。Web 登录默认开启，先登录一次并复用 Cookie：

   .. code-block:: bash

      export HUB=<hub-ip>          # 来自 eth got IP 日志
      curl -s -c /tmp/mh -X POST http://$HUB/api/auth/login \
        -H 'Content-Type: application/json' \
        -d '{"username":"admin","password":"secret1"}'

.. code-block:: bash

   curl -s -b /tmp/mh http://$HUB/api/modules
   curl -s -b /tmp/mh http://$HUB/api/packs
   curl -s -b /tmp/mh http://$HUB/api/config/adapters

串口日志：JS 模块输出 ``JS module '<id>' setup ok``；原生模块输出
``loaded module binary: <path>``。

升级 / 卸载
-----------

升级：

.. code-block:: bash

   curl -s -b /tmp/mh -X POST http://$HUB/api/store/upgrade \
     -H 'Content-Type: application/json' \
     -d '{"type":"module","id":"color_bulb_js"}'

卸载：

.. code-block:: bash

   curl -s -b /tmp/mh -X POST http://$HUB/api/store/uninstall \
     -H 'Content-Type: application/json' \
     -d '{"type":"module","id":"color_bulb_js"}'

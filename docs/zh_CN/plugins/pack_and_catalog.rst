打包与目录
==========

:link_to_translation:`en:[English]`

本章说明 Store 模块从 **开发机到 Hub** 的完整流水线。

前置条件
--------

* L1 / L2 打包不需要编 Host 固件
* L3 **仓库外** 交叉编译：只要有 MHMD ``.so`` （见 :doc:`develop_l3` ），不必编固件
* L3 **仓库内** 跟固件一起编：产品须打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES``
  （ ``app.yml`` 的 ``dynamic_modules`` ），否则 CMake 不会生成
  ``libadapter_*.so``
* Hub **加载** L2 / L3 时，烧进去的固件也必须打开上述选项
* 脚本缺 PyYAML 时在仓库根目录 ``pip install pyyaml``
* 可选：``export MINIHUB_PLUGINS_DEPLOY_DIR=~/workspace/minihub_store``

打包命令
--------

.. code-block:: bash

   # L3 仓库内：编固件（成功后会自动 pack .mhub）
   ./tools/build_firmware.py -app minihub build

   # 编译全部仓库内原生插件 .so（不打包）
   ./tools/build_firmware.py -app minihub modules

   # 已有仓库内 .so 时只再打包（不编译 C）
   ./tools/build_firmware.py -app minihub pack

   # 任意层级：只打 zip，不编译 C
   python3 tools/pack_store_module.py pack ID
   python3 tools/pack_store_module.py pack all

   # 手动 MHMD（通常 pack 已包含）
   python3 tools/pack_store_module.py mhmd in.so out.so

   # 仓库外演示：无 IDF 编译
   python3 tools/build_plugin_standalone.py color_bulb

   # .mhub 已在 store 目录后，按 zip 重写 catalog.json（默认；不需要插件源码）
   python3 tools/sync_store_catalog.py
   python3 tools/sync_store_catalog.py --deploy ~/workspace/minihub_store

   # 按插件源码打包，再同样重写 catalog
   python3 tools/sync_store_catalog.py all
   python3 tools/store_server.py

``./tools/build_firmware.py … modules`` 编译同时含 ``module.json`` 与
``CMakeLists.txt`` 的全部原生插件 （``examples/plugins/*/`` 以及
``MINIHUB_PLUGIN_DIRS``）。它不打包。``./tools/build_firmware.py … pack``
把已有的 ``libadapter_ID.so`` 封装成 MHMD 和 ``Stores/ESP32S31/ID/ID.mhub``
（ ``targets`` 为 ``["ALL"]`` 时是 ``Stores/Common`` ），并回写
``catalog.json`` 的 **size**。固件 ``build`` 成功后也会自动跑这套打包
（源码须在 CMake 路径上）。

``.mhub`` 已经放进 store 目录后（例如
``~/workspace/minihub_store/Stores/ESP32S31/ble_mesh_pack/ble_mesh_pack.mhub`` ），运行
``sync_store_catalog.py`` （ ``--from-store`` 是默认行为）。它遍历
``Stores/ESP32S31/ID/ID.mhub`` 和 ``Stores/Common/ID/ID.mhub`` ，读取 zip 内的 ``module.json`` （有则一并读
``ui.json`` ），在该包旁边写出 ``catalog.json`` 。不需要插件源码。同一脚本也在
``module_sdk.zip`` 里。 ``GET /api/catalog`` 聚合这些对象；只拷贝 zip
不会更新目录。默认部署根是 ``MINIHUB_PLUGINS_DEPLOY_DIR`` 或
``~/workspace/minihub_store`` 。

部署目录布局
------------

.. code-block:: text

   DEPLOY/Stores/ESP32S31/ID/catalog.json
   DEPLOY/Stores/ESP32S31/ID/ID.mhub
   DEPLOY/Stores/Common/ID/catalog.json
   DEPLOY/Stores/Common/ID/ID.mhub

默认 ``store_server.py`` 监听 ``http://0.0.0.0:8080``
(可用 ``--port`` 或 ``MINIHUB_STORE_PORT`` 覆盖)。

外层容器：**zip** (``MHUB_PACK_FORMAT``，默认 ``zip``)。Host 解压：
``components/minihub_store/hub_zip.c`` (目前仅 zip)。

**MHMD** 是内层原生镜像魔数 (``0x4D484D44``)，不是外层 zip 名称。

``catalog.json`` 条目 (由 ``sync_store_catalog.py`` 生成)
--------------------------------------------------------------

.. code-block:: json

   {
     "kind": "module",
     "id": "color_bulb",
     "name": "Color Bulb (simulated)",
     "version": "0.1.0",
     "targets": ["ESP32S31"],
     "url": "/files/Stores/ESP32S31/color_bulb/color_bulb.mhub",
     "adapter": "color_bulb",
     "size": 3762,
     "binary_size": 12000
   }

* ``kind`` — L2/L3 为 ``module``；纯配置 L1 可为 ``pack``
  (仅 JSON 包时 ``module`` / ``pack`` 可互换)
* ``url`` — ``.mhub`` 的路径或绝对 HTTP URL
* ``targets`` — 芯片文件夹名，与 ``Firmwares/<chip>/`` 使用同一套字符串
  （例如 ``ESP32S31`` ），JS 模块也可以是 ``ALL`` 。从 ``module.json``
  抄入；已经写了的列表原样保留。省略该字段时，JS 模块打包写成
  ``["ALL"]`` ，其它模块写成 ``["ESP32S31"]``
* ``adapter`` — 可选；安装时 Host 会在 ``adapters.json`` 中建槽
* ``size`` — **外层 zip** 字节数（须与磁盘上 ``.mhub`` 一致；``pack`` 会回写）
* ``binary_size`` — 内层 MHMD ``.so`` 大小（不是 zip）

在运行中的 Hub 上安装
---------------------

**本地 Store 服务：** Web UI → Store → 刷新目录 → 安装。

**REST** (仅 JSON，无 multipart 文件上传)：

.. code-block:: bash

   HUB=192.168.1.10

   curl -X POST http://$HUB/api/store/config \
     -H 'Content-Type: application/json' \
     -d '{"custom_url":"http://YOUR_PC:8080","disable_default":false}'

   curl -X POST http://$HUB/api/store/refresh
   curl -X POST http://$HUB/api/store/install \
     -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"color_bulb"}'

   curl -X POST http://$HUB/api/hub/reload

L1 包使用 ``{"kind":"pack","id":"my_pack"}``。

验证安装
--------

.. code-block:: bash

   curl http://$HUB/api/modules
   curl http://$HUB/api/packs
   curl http://$HUB/api/config/adapters

串口：``loaded module 'color_bulb'`` 或 L2 的 ``setup ok``。

升级 / 卸载
-----------

升级：

.. code-block:: bash

   curl -X POST http://$HUB/api/store/upgrade -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"color_bulb"}'

卸载：

.. code-block:: bash

   curl -X POST http://$HUB/api/store/uninstall -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"color_bulb"}'

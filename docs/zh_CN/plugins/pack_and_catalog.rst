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

   # 已有仓库内 .so 时只再打包（不要写成 build modules）
   ./tools/build_firmware.py -app minihub modules

   # 任意层级：只打 zip，不编译 C
   python3 tools/pack_store_module.py pack ID
   python3 tools/pack_store_module.py pack all

   # 手动 MHMD（通常 pack 已包含）
   python3 tools/pack_store_module.py mhmd in.so out.so

   # 仓库外演示：无 IDF 编译
   python3 tools/build_plugin_standalone.py color_bulb

   # .mhub 已在 store 目录后，按 zip 重写 catalog.json（不需要插件源码）
   python3 tools/sync_store_catalog.py --from-store
   python3 tools/sync_store_catalog.py --from-store --deploy ~/workspace/minihub_store

   # 仍有插件源码时刷新目录（同时会重新打包）
   python3 tools/sync_store_catalog.py all
   python3 tools/store_server.py

``./tools/build_firmware.py … modules`` 扫描同时含 ``module.json`` 与
``CMakeLists.txt`` 的插件目录：``components/minihub_plugins/*/`` 以及
``MINIHUB_PLUGIN_DIRS`` （``:`` 或 ``;`` 分隔）。链接
``libadapter_ID.so``，封装 MHMD，输出 ``DEPLOY/ID/ID.mhub``，并回写
``catalog.json`` 的 **size** 与 zip 一致。固件 ``build`` 成功后也会自动跑
同一套打包（源码须在 CMake 路径上）。

``.mhub`` 已经放进 store 目录后（例如
``~/workspace/minihub_store/ble_mesh_pack/ble_mesh_pack.mhub`` ），运行
``sync_store_catalog.py --from-store`` 。它遍历每个 ``DEPLOY/ID/ID.mhub`` ，
读取 zip 内的 ``module.json`` （有则一并读 ``ui.json`` ），写出
``catalog.json`` 。不需要插件源码。同一脚本也在 ``module_sdk.zip`` 里。
本地 Store 服务读的是这份目录文件；只拷贝 zip 不会更新目录。默认部署根是
``MINIHUB_PLUGINS_DEPLOY_DIR`` 或 ``~/workspace/minihub_store`` 。

部署目录布局
------------

.. code-block:: text

   DEPLOY/catalog.json
   DEPLOY/ID/ID.mhub

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
     "id": "miot",
     "name": "Xiaomi MIoT adapter",
     "version": "0.1.0",
     "url": "/files/miot.mhub",
     "adapter": "miot",
     "size": 3762,
     "binary_size": 12000
   }

* ``kind`` — L2/L3 为 ``module``；纯配置 L1 可为 ``pack``
  (仅 JSON 包时 ``module`` / ``pack`` 可互换)
* ``url`` — ``.mhub`` 的路径或绝对 HTTP URL
* ``adapter`` — 可选；安装时 Host 会在 ``adapters.json`` 中建槽
* ``size`` — **外层 zip** 字节数（须与磁盘上 ``.mhub`` 一致；``pack`` /
  ``modules`` 会回写）
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
     -d '{"kind":"module","id":"miot"}'

   curl -X POST http://$HUB/api/hub/reload

L1 包使用 ``{"kind":"pack","id":"my_pack"}``。

验证安装
--------

.. code-block:: bash

   curl http://$HUB/api/modules
   curl http://$HUB/api/packs
   curl http://$HUB/api/config/adapters

串口：``loaded module 'miot'`` 或 L2 的 ``setup ok``。

升级 / 卸载
-----------

升级：

.. code-block:: bash

   curl -X POST http://$HUB/api/store/upgrade -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"miot"}'

卸载：

.. code-block:: bash

   curl -X POST http://$HUB/api/store/uninstall -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"miot"}'

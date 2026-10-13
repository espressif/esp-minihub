部署索引
========

:link_to_translation:`en:[English]`

PC 部署根（``MINIHUB_PLUGINS_DEPLOY_DIR`` ，默认 ``~/workspace/minihub_store`` ）
集中存放插件、固件、资源与 Web 控制台。``python3 tools/store_server.py
--deploy ROOT`` 在该目录上以 8080 端口（``MINIHUB_STORE_PORT`` ）提供服务。
完整的索引格式与 ``/api/*`` 协议以仓库根目录的
``minihub_website_internal/README.txt`` 为单一真相。

插件
----

每个模块都作为一个 ``.mhub`` 发布。L1/L2 放在 ``Stores/Common/<id>/`` 。``catalog.json``
是打包 ``.mhub`` 时由 ``tools/sync_store_catalog.py`` 写在同目录的单个对象。

.. code-block:: text

   ROOT/Stores/Common/ID/ID.mhub
   ROOT/Stores/Common/ID/catalog.json

对象包含 ``size`` 、``sha256`` 、``url`` 、``targets`` 。``url`` 与目录一致，例如
``/files/Stores/Common/ID/ID.mhub`` 。``GET /api/catalog`` 遍历
``Stores/Common`` ，返回兼容的插件（见下）。Hub 商店客户端下载响应中的 ``url`` 。

一个插件 catalog 条目形如（由 ``tools/sync_store_catalog.py`` 依据构建出的
``.mhub`` 生成）：

.. code-block:: json

   {
     "type": "module", "id": "foo", "name": "Foo", "version": "1.0.0",
     "targets": ["ALL"], "url": "/files/Stores/Common/foo/foo.mhub",
     "size": 1234, "sha256": "0123abcd..."
   }

``sha256`` 是 ``.mhub`` 的 64 位十六进制摘要。插件
按构建结果即兼容：服务端只按 ``targets`` （``?chip=`` ）过滤，设备再本地
过滤一次作为纵深防御。

catalog 条目字段
----------------

``catalog.json`` 每个模块一个对象，字段含义：

* ``type`` —— 固定为 ``"module"`` 。
* ``id`` —— 模块 id（与目录名、``.mhub`` 名一致）。
* ``name`` —— 显示名；缺省用 ``id`` 。
* ``version`` —— 来自 module.json 的模块版本；缺省 ``1.0.0`` 。
* ``vendor`` —— 来自 module.json 的厂商字符串；缺省为空。
* ``description`` —— module.json 的 ``description`` 。
* ``adapter`` —— Host 会创建/启用的 ``adapters.json`` 键；取自 ``adapter`` 、
  ``inject.adapter`` 或首个 ``requires`` 项。
* ``url`` —— ``/files/Stores/<bucket>/<id>/<id>.mhub`` ，下载路径。
* ``targets`` —— 芯片目录名列表（如 ``["esp32s31"]`` 或 ``["ALL"]`` ），即
  ``?chip=`` 的过滤依据。
* ``runtime`` —— ``"pack"`` （纯配置 L1）或 ``"js"`` （L2）。
* ``config_only`` —— 仅纯配置 L1 包为 ``true`` 。
* ``size`` —— ``.mhub`` 的字节数。
* ``sha256`` —— ``.mhub`` 的 64 位十六进制摘要。

``ui`` 与 ``capabilities`` 不是 catalog 字段。它们放在 ``.mhub`` 内的
``meta.json`` 中。

设备请求在插件 catalog、``/api/firmware`` 、``/api/assets`` 与 ``/api/web`` 上
附带 ``mac`` （小写、冒号分隔）、``chip`` （小写，设备 ``CONFIG_IDF_TARGET`` ）
与 ``cur_version`` （设备当前运行的固件版本）。服务端按 chip 过滤目录与固件，
设备再本地过滤一次。每个插件下载、固件 ``.bin`` 、各资源文件与各 web 文件同样
附带 ``mac`` 、``chip`` 与 ``cur_version`` 。

拉取协议（``/api/catalog``）
----------------------------------------

查询携带 ``mac`` （小写、冒号分隔）、 ``chip`` （小写，设备的
``CONFIG_IDF_TARGET`` ），可附带 ``cur_version`` （设备当前运行的固件版本）。
服务端按 ``chip`` 过滤目录条目，设备再在本地过滤一次作为纵深防御。

* ``mac`` —— 仅用于按设备的强制授权（overrides）。
* ``chip`` —— 设备的芯片；服务端据此丢弃 ``targets`` 指向其他芯片的条目。
* ``cur_version`` —— 可选。服务端据此判断是否有比该设备更新的固件
  （见 ``firmware_upgrade.newer`` ）。

服务端遍历 ``Stores/Common`` ，返回每个插件 id 的最新构建。零匹配时 ``status``
为 ``no_plugin_for_version`` ， ``plugins`` 为空。

``firmware_upgrade.version`` 是该芯片固件目录下版本号最高的 MiniHub
固件目录名。 ``available`` 为真表示该版本目录里有 ``.bin`` 。
``firmware_url`` 指向这个文件。若请求带了 ``cur_version`` ， ``newer`` 为真表示
已发布版本比设备当前更新；设备安装固件时不读这个字段，判定在下面的
``GET /api/firmware`` 。

请求示例
--------

Store 默认听在 ``http://127.0.0.1:8080`` 。

.. code-block:: bash

   curl -s "http://127.0.0.1:8080/api/catalog?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"

HTTP 200 。匹配的插件与已发布的最高固件版本：

.. code-block:: json

   {
     "status": "ok",
     "plugins": [
       {
         "id": "foo",
         "name": "Foo",
         "version": "1.0.0",
         "targets": ["esp32s31"],
         "url": "/files/Stores/Common/foo/foo.mhub",
         "size": 1234,
         "sha256": "ab93bfdd952f04b5c47b7ef1733e3774aaa1b852a453f6ad7b53437a01647ffe"
         }
     ],
     "firmware_upgrade": {
       "available": true,
       "version": "0.1.0",
       "firmware_url": "/files/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin"
     }
   }

包的 ``sha256`` 是 64 位十六进制。按返回的 ``url`` 下载包：

.. code-block:: bash

   curl -s -o foo.mhub "http://127.0.0.1:8080/files/Stores/Common/foo/foo.mhub?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31"

固件
----

每个芯片与版本一个目录，目录内无 JSON：

.. code-block:: text

   ROOT/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin
   ROOT/Firmwares/esp32s31/0.1.0/release_notes.txt
   ROOT/Firmwares/esp32s31/0.1.0/bootloader.bin
   ROOT/Firmwares/esp32s31/0.1.0/partition-table.bin
   ROOT/Firmwares/esp32s31/0.1.0/merged.bin
   ROOT/Firmwares/esp32s31/0.1.0/minihub.elf
   ROOT/Firmwares/esp32s31/0.1.0/flash_command.txt

``python3 tools/publish_firmware.py`` 拷贝应用 ``.bin`` 与说明。
``python3 tools/gen_flash_binaries.py`` 把其余烧录文件放进同一目录。芯片目录名为小写。

``GET /api/firmware?mac=&cur_version=&chip=`` 返回 JSON 数组。设备以 ``chip``
（小写）表明自身；服务端只列出该芯片的固件（纵深防御——设备也会按 ``targets``
在本地过滤）。 ``cur_version`` 可选，缺省仍会返回。请求同样携带 ``mac`` 。
数组只列出应用镜像：

.. code-block:: bash

   curl -s "http://127.0.0.1:8080/api/firmware?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"
   curl -s -o minihub.bin "http://127.0.0.1:8080/files/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin?mac=aa:bb:cc:dd:ee:ff&cur_version=0.1.0"

.. code-block:: json

   [
     {
       "id": "esp32s31-0.1.0",
       "name": "minihub-esp32s31-0.1.0",
       "version": "0.1.0",
       "chip": "esp32s31",
       "targets": ["esp32s31"],
       "sha256": "0123abcd...",
       "size": 1234,
       "release_note": "first cut",
       "url": "/files/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin"
     }
   ]

* ``version`` —— 版本目录名。
* ``chip`` 与 ``targets`` —— 芯片目录名。
* ``sha256`` —— ``.bin`` 的 64 位十六进制摘要。
* ``size`` —— 字节数。
* ``release_note`` —— 同目录 ``release_notes.txt`` 的正文。
* ``url`` —— 部署根内的路径。同一版本目录里有多份应用 ``.bin`` 时， ``id`` 会再带上文件名。

设备上 ``minihub_update_check()`` 与 ``minihub_update_apply()`` 的步骤：

1. 请求上面的数组，并附带 ``mac`` 与 ``version`` 。
2. 用本机芯片名过滤 ``targets`` （不区分大小写），留下版本号最高的一项。比较时去掉
   版本号开头的 ``v`` 。
3. 该项版本高于 ``CONFIG_MINIHUB_VERSION`` （没有该配置时用应用描述里的版本）则下载。
   版本相同，但 ``size`` 或 ``sha256`` 与上次下载记录不同，也会再下一次。
4. ``minihub_ota_start_url()`` 打开 ``url`` ，并附带 ``mac`` 与 ``version`` 。下载时用算出的 64 位
   十六进制核对 ``sha256`` ，不一致则丢弃。随后写入空闲 OTA 槽并重启，见 :doc:`../ota` 。

资源与 web
----------

``python3 tools/gen_deploy_index.py --deploy ROOT`` 读取设备部件源的
``model/*.csv`` 与 ``images/*`` ，产出设备类型查找表：

* ``Assets/device_widgets.json`` —— 按 ``device_id`` ：设备图标 + 每个参数的图标/样式/元数据 URL。
* ``Assets/parameters.json`` —— 按参数名索引的全局参数注册表（图标/样式/元数据）。
* ``Assets/styles/<param>.css`` —— 由 ``parameters.csv`` 的 ``css`` 列抽取，每参数一个 CSS 文件。

Web 与资源清单（schema ``minihub.deploy.index/1`` ）由 ``store_server.py`` 在请求时动态提供：
``GET /api/web`` 与 ``GET /api/assets`` 分别扫描 ``Webs/<version>/`` 与 ``Assets/`` 生成，
因此不再写静态 ``index.json`` （与 ``GET /api/firmware`` 一致）。

``--only assets`` 仅重建设备类型查找表（落单图标/样式后重跑即可，无需重建固件或插件）。
``--sync-source`` 先把 ``model/`` 与 ``images/`` 从设备部件组件拷入部署根。

.. code-block:: text

   ROOT/Assets/device_widgets.json
   ROOT/Assets/parameters.json
   ROOT/Assets/styles/<param>.css
   ROOT/Assets/model/*.csv
   ROOT/Assets/images/{devices,parameters}/*.svg
   ROOT/Webs/<version>/...   （清单由 GET /api/web 提供）

.. code-block:: bash

   curl -s "http://127.0.0.1:8080/api/assets?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"
   curl -s "http://127.0.0.1:8080/api/assets/device/light?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"
   curl -s "http://127.0.0.1:8080/api/web?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"

设备类型资源按需拉取：

* ``GET /api/assets/device/<device_id>`` —— 该设备图标 + 各参数图标/样式/元数据；未知返回 404。
* ``GET /api/assets/parameter/<param>`` —— 通用参数条目。
* ``GET /files/...`` —— 下发整棵树并拒绝 ``..`` / 越界。

设备上 ``components/minihub_update`` 复用 HTTP 客户端，在本地注册表未知某类型时拉取
``/api/assets/device/<id>`` ，把图标缓存到 ``/data/images`` 、样式到 ``/data/styles`` ；
渲染层只读本地存储，保持解耦。

``GET /files/...`` 映射到部署根，拒绝包含 ``..`` 或越界的路径。

设备侧 ``minihub_update_check()`` 与 ``minihub_update_apply()``
（``components/minihub_update`` ）读取 ``/api/firmware`` 、``/api/assets`` 、
``/api/web`` 。固件调用 ``minihub_ota_start_url()`` 。``devices.csv`` 与
``parameters.csv`` 经 ``minihub_device_widget_put_model()`` 进入 ``/data/config/model`` ，
其余模型文件落该目录，图片落 ``/data/images`` ，web 文件落 ``/data/web`` 。下载边写边校验哈希，不匹配则丢弃。

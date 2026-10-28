部署索引
========

:link_to_translation:`en:[English]`

PC 部署根（``MINIHUB_PLUGINS_DEPLOY_DIR``，默认 ``~/workspace/minihub_store`` ）
集中存放插件、固件、资源与 Web 控制台。``python3 tools/store_server.py
--deploy ROOT`` 在该目录上以 8080 端口（ ``MINIHUB_STORE_PORT`` ）提供服务。
完整的索引格式与 ``/api/*`` 协议以仓库根目录的
``minihub_website_internal/README.txt`` 为单一真相。

插件
----

发布出的每个模块都是一个 ``.mhub``。L1 与 L2 放在 ``Stores/Common/<id>/``。原生
与 hybrid 模块不是商店包： ``store_buckets_for_module()`` 不给它们分配目录，残留
目录会被删除，所以部署树里只有 L1/L2。``catalog.json``
是打包 ``.mhub`` 时由 ``tools/sync_store_catalog.py`` 写在同目录的单个对象。

.. code-block:: text

   ROOT/Stores/Common/ID/ID.mhub
   ROOT/Stores/Common/ID/catalog.json

``url`` 与目录一致，例如 ``/files/Stores/Common/ID/ID.mhub``。``GET /api/catalog`` 遍历
``Stores/Common``，返回兼容的插件（见下）。Hub 商店客户端下载响应中的 ``url``。

一个插件 catalog 条目形如（由 ``tools/sync_store_catalog.py`` 依据构建出的
``.mhub`` 生成）：

.. code-block:: json

   {
     "type": "module", "id": "foo", "name": "Foo", "version": "1.0.0",
     "targets": ["ALL"], "url": "/files/Stores/Common/foo/foo.mhub",
     "size": 1234, "sha256": "0123abcd..."
   }

决定一个插件能否安装的是：``targets`` （``?chip=`` ），服务端过滤一次、设备本地
再过滤一次。服务端的 ``abi`` 校验只在请求同时带上 ``abi_version`` 与
``abi_total_hash`` 时才执行，而 Hub 不发这两个参数；JS 与纯配置条目不带
``abi`` 块，始终视为兼容。Hub 上真正生效的是本地两步：安装前校验包引用的
Host 符号，加载前校验 ``abi_version`` / ``core_api_min`` / ``core_api_max``。

catalog 条目字段
----------------

``catalog.json`` 每个模块一个对象，字段含义：

* ``type`` —— 固定为 ``"module"``。
* ``id`` —— 模块 id（与目录名、``.mhub`` 名一致）。
* ``name`` —— 显示名；缺省用 ``id``。
* ``version`` —— 来自 module.json 的模块版本；缺省 ``1.0.0``。
* ``vendor`` —— 来自 module.json 的厂商字符串；缺省为空。
* ``description`` —— module.json 的 ``description``。
* ``adapter`` —— Host 会创建/启用的 ``adapters.json`` 键；取自 ``adapter``、
  ``inject.adapter`` 或首个 ``requires`` 项。
* ``url`` —— ``/files/Stores/<bucket>/<id>/<id>.mhub``，下载路径。
* ``targets`` —— 芯片目录名列表（如 ``["esp32s31"]`` 或 ``["ALL"]`` ），即
  ``?chip=`` 的过滤依据。
* ``runtime`` —— ``"pack"`` （纯配置 L1）或 ``"js"`` （L2）。``"hybrid"`` 与
  ``"native"`` 描述的是原生模块，二者都不发布。
* ``config_only`` —— 仅纯配置 L1 包为 ``true``。
* ``size`` —— ``.mhub`` 的字节数。
* ``sha256`` —— ``.mhub`` 的 64 位十六进制摘要。
* ``host_api`` —— 仅 L2 与 hybrid：取自 ``module.json``，缺省为 ``0``。
* ``abi`` —— 仅原生与 hybrid：``abi_version`` 加 ``symbols`` （包所引用的 Host
  符号哈希）。Hub 的部署树里不会出现该条目，因为它不发布原生或 hybrid 包。

``ui`` 与 ``capabilities`` 不是 catalog 字段。它们留在包内 ``meta.json``。
安装后控制台从 ``GET /api/modules`` 读取。

设备查询参数
--------------

所有设备请求 —— ``/api/catalog``、 ``/api/firmware``、 ``/api/assets``、
``/api/web`` 与每个 ``/files/…`` 下载 —— 都带同样的三个查询参数：

* ``mac`` —— 小写、冒号分隔。只用于服务端按设备的强制授权（overrides）；不带
  ``mac`` 的请求照常返回，只在访问日志里记下缺失。
* ``chip`` —— 设备的小写 ``CONFIG_IDF_TARGET``。服务端据此丢弃 ``targets``
  指向其他芯片的条目；不带 ``chip`` 的请求不做过滤。
* ``cur_version`` —— 设备当前运行的固件版本，可选。

服务端按 ``chip`` 过滤，设备再对结果本地过滤一次，作为纵深防御。

拉取协议（``/api/catalog``）
----------------------------------------

服务端遍历 ``Stores/Common``，返回每个插件 id 的最新构建；零匹配时 ``status``
为 ``no_plugin_for_version``， ``plugins`` 为空。

``firmware_upgrade.version`` 是该芯片固件目录下版本号最高的 MiniHub
固件目录名。 ``available`` 为真表示该版本目录里有 ``.bin``。
``firmware_url`` 指向这个文件。若请求带了 ``cur_version``， ``newer`` 为真表示
已发布版本比设备当前更新；设备安装固件时不读这个字段，判定在下面的
``GET /api/firmware``。

请求示例
--------

商店默认听在 ``http://127.0.0.1:8080``。

.. code-block:: bash

   curl -s "http://127.0.0.1:8080/api/catalog?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"

HTTP 200。匹配的插件与已发布的最高固件版本：

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

按返回的 ``url`` 下载包：

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

``GET /api/firmware`` 返回 JSON 数组，逐项列出请求的 ``chip`` 可运行的应用
镜像（设备会再按 ``targets`` 过滤一次）：

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

1. 请求上面的数组，附带 ``mac``、``chip`` 与 ``cur_version``。
2. 用本机芯片名过滤 ``targets`` （不区分大小写），留下版本号最高的一项。比较时去掉
   版本号开头的 ``v``。
3. 该项版本高于 ``CONFIG_MINIHUB_VERSION`` （没有该配置时用应用描述里的版本）则下载。
   版本相同，但 ``size`` 或 ``sha256`` 与上次下载记录不同，也会再下一次。
4. ``minihub_ota_start_url()`` 打开 ``url``，并附带 ``mac`` 与 ``cur_version``。固件
   下载流 **不** 按 ``sha256`` 比对：摘要只是第 3 步用的元数据，作用是让同一版本号但
   重新构建过的镜像被再下一次。真正校验镜像的是写完后调用的 ``esp_ota_end()``，它检查
   镜像头与序列号校验和。随后镜像落在空闲 OTA 槽并重启，见 :doc:`../ota`。只有资源与
   控制台文件的下载才会先比 ``sha256`` 再替换。

资源与 web
----------

``python3 tools/gen_deploy_index.py --deploy ROOT`` 读取设备部件源的
``model/*.csv`` 与 ``images/*``，产出设备类型查找表：

* ``Assets/device_widgets.json`` —— 按 ``device_id`` ：设备 ``icon`` ，外加每个参数的
  ``icon`` 与 ``meta``。
* ``Assets/parameters.json`` —— 按参数名索引的全局参数注册表（``icon`` / ``meta``）。

不发布按参数生成的样式表。``parameters.csv`` 的 ``css`` 列存的是 **类名** ，不是 CSS 文本：
控制台把它交给 ``dwCssClass()`` ，而规则本身在随 ``GET /api/device_widgets`` 一起下发的
``device_widgets.css`` 里。样式表仍可经开发面板手动推上去
（``PUT /api/dev/assets/styles/...``）。

Web 与资源清单（schema ``minihub.deploy.index/1`` ）由 ``store_server.py`` 在请求时动态提供：
``GET /api/web`` 与 ``GET /api/assets`` 分别扫描 ``Webs/<version>/`` 与 ``Assets/`` 生成，
因此不再写静态 ``index.json`` （与 ``GET /api/firmware`` 一致）。

``--only assets`` 仅重建设备类型查找表（放进一个新图标后重跑即可，无需重建固件或插件）。
``--sync-source`` 先把 ``model/`` 与 ``images/`` 从设备部件组件拷入部署根。

.. code-block:: text

   ROOT/Assets/device_widgets.json
   ROOT/Assets/parameters.json
   ROOT/Assets/model/*.csv
   ROOT/Assets/images/{devices,parameters}/*.svg
   ROOT/Webs/<version>/...   （清单由 GET /api/web 提供）

.. code-block:: bash

   curl -s "http://127.0.0.1:8080/api/assets?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"
   curl -s "http://127.0.0.1:8080/api/assets/device/light?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"
   curl -s "http://127.0.0.1:8080/api/web?mac=aa:bb:cc:dd:ee:ff&chip=esp32s31&cur_version=0.1.0"

设备类型资源按需拉取：

* ``GET /api/assets/device/<device_id>`` —— 该设备图标 + 各参数的图标与元数据；未知返回 404。
* ``GET /api/assets/parameter/<param>`` —— 通用参数条目。
* ``GET /files/...`` —— 下发整棵树并拒绝 ``..`` / 越界。

设备上 ``components/minihub_update/update_assets.c`` **不会** 先要元数据：头像按约定就是
``<type>.svg`` ，所以直接请求 ``GET /files/Assets/images/devices/<type>.svg`` ，存到
``/data/images/devices/<type>.svg`` 。这是有意的——元数据路由由 ``model/devices.csv`` 驱动，
插件自有的类型不在其中，会返回 404，那样的类型就永远拿不到图标。设备上已有的文件不会被覆盖
（先 ``stat`` ），所以类型取过一次之后每小时的任务不再产生流量；服务端重画图标也到不了已有文件的设备。
参数图标不拉取。

``GET /files/...`` 映射到部署根，拒绝包含 ``..`` 或越界的路径。

设备侧 ``minihub_update_check()`` 与 ``minihub_update_apply()``
（``components/minihub_update`` ）读取 ``/api/firmware``、``/api/assets``、
``/api/web``。固件调用 ``minihub_ota_start_url()``。``devices.csv`` 与
``parameters.csv`` 经 ``minihub_device_widget_put_model()`` 进入 ``/data/config/model``，
其余模型文件落该目录，图片落 ``/data/images``，web 文件落 ``/data/web``。下载边写边校验哈希，不匹配则丢弃。

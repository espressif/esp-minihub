部署索引
========

:link_to_translation:`en:[English]`

PC 部署根（``MINIHUB_PLUGINS_DEPLOY_DIR`` ，默认 ``~/workspace/minihub_store`` ）
集中存放插件、固件、资源与 Web 控制台。``python3 tools/store_server.py
--deploy ROOT`` 在该目录上以 8080 端口（``MINIHUB_STORE_PORT`` ）提供服务。
完整的索引格式、``/api/*`` 协议与 ABI 哈希方案以仓库根目录的
``minihub_website_internal/README.txt`` 为单一真相。

插件
----

每个模块一个目录。``catalog.json`` 是打包 ``.mhub`` 时由
``tools/sync_store_catalog.py`` 写入的单个对象。具体芯片落到对应芯片目录；
``targets`` 为 ``["ALL"]`` 的（JS）模块留在 ``Common`` 。多个具体芯片各拷一份。

.. code-block:: text

   ROOT/Stores/ESP32S31/ID/ID.mhub
   ROOT/Stores/ESP32S31/ID/catalog.json
   ROOT/Stores/Common/ID/ID.mhub
   ROOT/Stores/Common/ID/catalog.json

对象包含 ``size`` 、``sha256`` 、``url`` 、``targets`` 。``url`` 与目录一致，例如
``/files/Stores/Common/ID/ID.mhub`` 。``GET /api/catalog`` 遍历各芯片目录与
``Stores/Common`` ，返回兼容的插件（见下）。Hub 商店客户端下载响应中的 ``url`` 。

每个插件 catalog 都带有由 ``tools/sync_store_catalog.py`` 依据构建出的
``.mhub`` 生成的 ``abi`` 块：

.. code-block:: json

   {
     "id": "foo", "name": "Foo", "version": "1.0.0", "targets": ["ALL"],
     "type": "plugin", "url": "/files/Stores/Common/foo/foo.mhub",
     "size": 1234, "sha256": "sha256:...",
     "abi": { "abi_version": 1,
              "symbols": { "host_api_x": "sha256:...", "host_api_y": "sha256:..." } }
   }

``symbols`` 把插件链接到的每个 Host API 映射到该符号当前的 Host 哈希，哈希由
``tools/gen_abi_contract.py`` 计算并烧录进固件，因此服务端、设备与插件三方一致。

``/api/catalog`` 在请求时聚合各插件的 ``catalog.json`` 对象，不写派生索引文件。

设备请求在插件 catalog、每个插件下载、``/api/firmware`` 与 ``.bin`` 、``/api/assets``
与各资源文件、``/api/web`` 与各 web 文件上附带 ``mac`` 查询（小写、冒号分隔）。
缺 ``mac`` 仍会服务，仅记录在访问日志。

拉取协议（``/api/catalog``）
----------------------------------------

查询：``?mac=&chip=&abi_version=&abi_total_hash=`` 。

* ``mac`` —— 仅用于按设备的强制授权（overrides）。
* ``chip`` —— 用于固件升级提示与 ``targets`` 过滤。
* ``abi_version`` + ``abi_total_hash`` —— 服务端执行两步 ABI 校验：

  1. **真实性：** ``abi_total_hash`` 必须等于
     ``host_abi_versions.json[abi_version].abi_total_hash`` ；否则返回
     ``{"status":"no_plugin_for_version","reason":"abi_mismatch"}`` ，不下发。
  2. **兼容性：** 对每个插件 id 保留最新版，仅当其每个 ``abi.symbols`` 哈希都落在
     Host 可接受集（``host_abi_versions.json`` 各符号的 ``acceptable`` ）内；缺哈希的插件被剔除。
     零兼容时返回 ``{"status":"no_plugin_for_version","firmware_upgrade":{...}}`` 。

响应：

.. code-block:: text

   { "status": "ok", "plugins": [ <catalog 对象> ],
     "firmware_upgrade": { "available": false, "min_abi_version": 0, "firmware_url": "" } }

``firmware_upgrade.available`` 为真表示 ``Firmwares/<chip>/`` 下存在更高 ``abi_version`` 的固件。
未带 ``abi_version``/``abi_total_hash`` 时退化为完整聚合 catalog（兼容旧设备）。

固件
----

每个芯片与版本一个目录，目录内无 JSON：

.. code-block:: text

   ROOT/Firmwares/ESP32S31/0.1.0/minihub-esp32s31-0.1.0.bin
   ROOT/Firmwares/ESP32S31/0.1.0/release_notes.txt

``python3 tools/publish_firmware.py`` 拷贝 ``.bin`` 与说明。``GET /api/firmware``
构建数组：``version`` 为目录名，``targets`` 为芯片目录，``sha256``/``size`` 由
``.bin`` 计算，``release_note`` 为文本，``url`` 为 ``/files/Firmwares/CHIP/VERSION/NAME.bin`` 。

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

设备类型资源按需拉取：

* ``GET /api/assets/device/<device_id>`` —— 该设备图标 + 各参数图标/样式/元数据；未知返回 404。
* ``GET /api/assets/parameter/<param>`` —— 通用参数条目。
* ``GET /files/...`` —— 下发整棵树并拒绝 ``..`` / 越界。

设备上 ``components/minihub_update`` 复用 HTTP 客户端，在本地注册表未知某类型时拉取
``/api/assets/device/<id>`` ，把图标缓存到 ``/data/images`` 、样式到 ``/data/styles`` ；
渲染层只读本地存储，保持解耦。设备同时烧录 ``minihub_abi_symbols.gen.h`` ，安装时按
其中可接受集校验每个插件的 ``abi.symbols`` （纵深防御）。

``GET /files/...`` 映射到部署根，拒绝包含 ``..`` 或越界的路径。

设备侧 ``minihub_update_check()`` 与 ``minihub_update_apply()``
（``components/minihub_update`` ）读取 ``/api/firmware`` 、``/api/assets`` 、
``/api/web`` 。固件调用 ``minihub_ota_start_url()`` 。``devices.csv`` 与
``parameters.csv`` 经 ``minihub_device_widget_put_model()`` 进入 ``/data/config/model`` ，
其余模型文件落该目录，图片落 ``/data/images`` ，web 文件落 ``/data/web`` 。下载边写边校验哈希，不匹配则丢弃。

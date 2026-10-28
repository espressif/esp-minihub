常见问题
========

:link_to_translation:`en:[English]`

厂商云集成应放在哪？
--------------------

作为商店插件（ ``.mhub`` ），不要写进 Host Core。见 :doc:`plugins/overview`。

如何添加自有 Host 产品？
------------------------

见 :doc:`application/index` 再 :doc:`host_customization`。复制 ``application/minihub`` 后用 ``-app APP``
（或 ``MINIHUB_APP`` ）编译。``build_firmware.py`` 会同步 manifest，不必再单独跑
``sync_host_manifest.py``。

商店插件装上了却什么都没出现
----------------------------

安装成功不等于加载成功。开机日志里找模块 id，``components/minihub_module/module_loader.c``
里这几行各自就是一个答案：

* ``JS module 'X' missing main.js`` （ :file:`module_loader.c:901` ）—— 包里没有
  ``main.js`` ，检查 ``module.json`` 的 ``runtime`` 是不是字面量 ``js`` 。
* ``module 'X' has no binary or builtin`` （ :file:`module_loader.c:906` ）—— Host 没有可入口的东西。
* ``skip module 'X' — adapter 'Y' already registered`` （ :file:`module_loader.c:912` ）——
  那个 ``adapter`` 名已被内置适配器占用。
* ``module table full, skip …`` （ :file:`module_loader.c:549` ）或 ``duplicate module id``
  （ :file:`module_loader.c:559` ）—— 容量已满或 id 重复。
* ``invalid module manifest`` （ :file:`module_loader.c:599` ）—— ``module.json`` 没解析成功，
  打包前先校验。
* ``main.js`` 里没有 ``setup`` —— 模块会被跳过并留下警告（ :doc:`api/js_host` ）。

再确认编译配置：JS 模块同时需要 ``CONFIG_MINIHUB_JS_PLUGINS`` 与
``CONFIG_MINIHUB_DYNAMIC_MODULES`` 。完整排查顺序见 :doc:`troubleshooting` 。

商店安装返回 400 / 404 / 409
------------------------------

* **400** — 缺少 ``type`` / ``id``。``type`` 只能是 ``module`` 或 ``pack``
  （不是 ``adapter`` ）。不支持 multipart ``file=@*.mhub``。
* **404** — 目录或制品缺失；正文只会写 ``ESP_ERR_NOT_FOUND``，不说原因，所以先设
  ``custom_url`` 再 ``POST /api/store/refresh``
* **409** — 已安装；用 ``POST /api/store/upgrade`` 或先卸载
* 检查 ``GET /api/store/config`` 中的商店基址 URL

这个端点能回的状态码与正文，全部见 :doc:`api/errors`。

.. only:: not hide_l3

   模块加载失败 (load_guard)
   -------------------------

   原生模块在 ``dlopen`` 时崩溃，Host 会写入 ``/store/modules/ID/.load_guard``，
   下次启动跳过。用 ``POST /api/modules`` 清除：

   .. code-block:: json

      { "id": "my_native", "clear_load_guard": true, "reload": true }

   串口查 ABI。L3 须让 ``module.json`` 的 ``abi_version`` 与 ``core_api_*`` 覆盖
   当前 Host（头文件 ``MINIHUB_MODULE_ABI_VERSION`` ）。见 :doc:`api/module_abi`。

Hub 重载循环或 MCP 报 HUB_NOT_READY
-----------------------------------

MCP ``hub.reload`` 限速 5 秒；REST ``POST /api/hub/reload`` 无此限速。重载期间
MCP 可能返回 ``HUB_NOT_READY``。Matter + Thread 构建在 CHIP 重新挂接时可能更久。

REST 返回 401 unauthorized
--------------------------

打开 ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认）时，REST 需要有效 ``mh_sess``
Cookie 或 ``Authorization: Bearer``。可选 token 用
``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` 决定（缺省为空，所以设备之间不共用同一个串），
非空时以它为准，只有编进来的串为空才读 NVS 键 ``api_token`` （命名空间 ``minihub`` ）。
关掉该选项则
局域网 REST 不鉴权。登录页见 :doc:`web_console`。

adapters.json 在哪？
---------------------

设备运行时： ``/data/config/adapters.json``。出厂配置：
``application/minihub/configs/adapters.json`` 或产品专用树。可通过 Web UI 或
``PUT /api/config/adapters`` 编辑，必要时 ``POST /api/hub/reload``。
完整 JSON 字段： :doc:`data_model`。

如何新增设备类型或改控件？
--------------------------

改 Host ``components/minihub_device_widget/model/*.csv`` （封闭
``ui_element``）后重编，**或** 在已运行 Hub 上上传两份 CSV
（``PUT /api/dev/catalog/devices.csv`` 与 ``parameters.csv``，需 Web UI）。
适配器仍 **一台设备登记一条点**，并把实时值写入 ``props.params``。
不要按类型加 C 结构。见 :doc:`type_catalog`。

如何替换 Web 控制台？
------------------------

改 ``components/minihub_web/web/`` 后 ``storage-flash``，或在已运行的 Hub 上
``PUT /api/dev/web/``。``app-flash`` 不会更新 ``/data/web``。见 :doc:`web_console`。

如何烧工厂镜像 / 做 OTA？
--------------------------

工厂镜像见 :doc:`production` （工装用 ``flash`` 或 ``build merge``；
``app-flash`` 会保留 ``/data`` 与 ``/store`` ）。现场 OTA 见 :doc:`ota`。

开源二次开发从哪里读起？
------------------------

左侧目录即完整结构。先读 :doc:`introduction` 与 :doc:`architecture`，用 AI
开发见 :doc:`vibe_coding`，再按交付物进入对应章节（OEM Host、商店插件或 API
集成）。贡献规范见 :doc:`contributing`。
本地构建 HTML 见 ``docs/README.md``。

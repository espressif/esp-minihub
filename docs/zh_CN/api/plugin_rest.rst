商店插件 REST API
===================

:link_to_translation:`en:[English]`

概述
----

HTTP JSON API，用于 JSON/JS 商店模块的开发、调试、安装与运维；插件 RPC 的 HTTP 入口。

实现：components/minihub_web + minihub_store + minihub_module。
需 Kconfig：CONFIG_MINIHUB_STORE、CONFIG_MINIHUB_DYNAMIC_MODULES。

.. only:: not hide_l3

   同一套 API 也覆盖 ELF 原生模块。加载它们需要
   ``CONFIG_MINIHUB_DYNAMIC_MODULES``。

Host 通用 REST 见 :doc:`host_rest`；状态码与错误正文见 :doc:`errors`。

通用约定
--------

* 没有 ``.mhub`` 上传接口 （不支持 multipart ``file=@*.mhub``）—— 先
  ``POST /api/store/refresh`` 刷新目录，再按 ``id`` 安装，由 Hub 下载 catalog 里的
  ``url``。
* type 仅允许 module（JS）或 pack（JSON）。

.. only:: not hide_l3

   ``type: module`` 也覆盖 ELF。

* 默认 reload: true，成功后触发 :doc:`../architecture` 中的 Hub 软重载。


GET /api/modules
----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules``

返回已安装商店模块列表及加载状态。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 数组；典型字段 id、name、version、enabled、loaded、
     runtime、adapter

   ``runtime`` 取值：

   * ``JS`` — JS QuickJS （``main.js``）

   无法分类时省略该字段。JSON pack 在 ``GET /api/packs``，不是
   ``runtime: pack``。

.. only:: not hide_l3

   ``runtime`` 也可以是 ``native`` （ELF ``.so``）或 ``hybrid`` （同时有原生与
   ``main.js``；可选 ``entry`` 为 ``JS`` 或 ``native``）。

POST /api/modules
-----------------

.. container:: mh-api-block mh-api-command

   ``POST /api/modules``

启用/禁用、删除模块，或清除 load_guard。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``id`` (string, required)
       模块 id。

   ``enabled`` (bool)
       启用或禁用。enabled、delete、clear_load_guard 设一个；同时出现时，
       delete 优先于 enabled，enabled 优先于 clear_load_guard。

   ``delete`` (bool)
       true 时删除包目录 ``/store/modules/ID/`` （二进制、``module.json``、
       ``ui.json``、``.load_guard``），并一并移除该模块在
       ``/data/config/adapters.json`` 里的槽位。模块私有的 NVS 键（账号、token）
       不动。

   ``clear_load_guard`` (bool)
       删除 ``.load_guard`` 后才会再次尝试加载。只有残留的 ``loading`` 会挡住
       自动加载 —— 它表示上次启动在该模块入口里崩了；``ok`` 不挡。

   ``reload`` (bool, optional)
       默认 true；操作成功后是否软重载 Hub。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok","id":"…","reloaded":true}``
   * **HTTP 400** — 缺少 id 或未指定三选一字段
   * **HTTP 500** — minihub_module_* 失败

POST /api/modules/ID/rpc
------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/modules/ID/rpc``

调用模块用 ``host.module.registerRpc`` 注册的 RPC 处理函数（见 :doc:`js_host`）。

.. only:: not hide_l3

   ELF 模块走 ``minihub_module_rpc``。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``method`` (string, required)
       RPC 方法名（模块自定义）。

   ``params`` (object, optional)
       JSON 参数。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — 模块定义的 JSON 对象
   * **HTTP 404** — 未知 method
   * **HTTP 400/500** — 参数或模块内部错误

.. container:: mh-api-block mh-api-attention

   **Attention**

   ``params`` 必须是 JSON 对象，请求体上限 16 KiB。调用在 JS worker 上执行，不占
   HTTP 任务，慢 RPC 不会卡住其他 API 请求。MCP hub.api 仅允许白名单 method
   代理本接口（:doc:`mcp`）。JS 模块需要 RPC 时，在 ``setup`` 里用
   ``host.module.registerRpc`` 注册。

GET /api/modules/ID/preview
---------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules/ID/preview``

模块可选预览资源（如摄像头帧）；响应类型因模块而异。

GET /api/packs · POST /api/packs
--------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/packs · POST /api/packs``

GET 返回已安装 JSON 配置包列表。POST 启用/禁用或删除 JSON 包（body 类似 POST /api/modules，
字段为 JSON id，无 clear_load_guard）。

GET /api/store/catalog
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/store/catalog``

返回当前 catalog 条目。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 数组；每项含 catalog 的 ``type``、``id``、``version``、
     ``url`` 等字段，再加 Hub 合并进来的三项：``installed``、
     ``installed_version``、``upgrade_available``

POST /api/store/refresh
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/refresh``

从 GET /api/store/config 配置的 URL 重新下载 catalog.json。下载在 HTTP 任务之外
执行，因此调用会在完成前返回。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"pending"}``；已有刷新进行中时同样是 200
     ``pending``，不会排第二次
   * **HTTP 500** — 后台任务无法启动

抓取本身有 5 秒超时；catalog 超过 64 KiB 或 JSON 非法，都会在状态接口里报成
``error``。

GET /api/store/refresh
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/store/refresh``

轮询 POST 启动的刷新。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``result`` 为 ``pending``、``idle``、``ok``、``aborted``
     （刷新途中商店 URL 被改动），或 ``error`` 并带 ``error`` 字符串

POST /api/store/install
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/install``

按 catalog 下载 ``.mhub`` 并解包。JS 模块先落到
``/store/modules/ID.staging/``，再 ``rename`` 成 ``/store/modules/ID/``，
所以不会出现解到一半就被看到的目录。JSON 包没有包目录：只保留它的清单 JSON，
位于 ``/data/config/packs/ID.json``。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``type`` (string, required)
       module 或 pack。

   ``id`` (string, required)
       catalog 中的 id。

   ``reload`` (bool, optional)
       默认 true。

   ``upgrade`` (bool, optional)
       true 时走升级路径，会重新检查版本门槛（见下面 ``409`` 那条）。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — 缺少 type/id 或请求体非法
   * **HTTP 404** — catalog 里没有该 id、制品不存在，或（``upgrade: true`` 时）
     该 id 还没装过
   * **HTTP 409** — 已安装且没走 upgrade；或升级时 catalog 版本并不更新
   * **HTTP 500** — 下载、解压、字节数或 ABI 校验失败

.. container:: mh-api-block mh-api-attention

   **Attention**

   完整性只看字节数，不摘要：下载长度必须等于 catalog 的 ``size``，catalog 里的
   ``sha256`` 设备端并不使用。下载不重试，一次失败就是 500。尺寸限制：包体不得超过
   ``CONFIG_MINIHUB_HUB_MAX_BYTES`` （默认 1 MiB，可配 64 KiB–4 MiB），zip 最多
   64 个条目、单条最大 2 MiB，``id`` 须短于 48 字符。版本比较是按点号取最多三段
   数字，其余情况用整串字典序兜底，所以 ``1.0.0-rc1`` 与 ``1.0.0`` 由字典序决定；
   已装版本超过 31 个字符会被截断，比较结果随之失真。

POST /api/store/upgrade · POST /api/store/uninstall
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/upgrade · POST /api/store/uninstall``

upgrade 只在 catalog 版本更新、或 ``binary_size`` / ``adapter_version`` 有差异时
覆盖；它不接受降级，重装同一版本或更旧的构建请用 ``reinstall``。uninstall 删除包
与适配器槽位；删除一个本来就没装的条目不算错误，因此这条接口只有 **500** 一种
失败状态。body：type + id 必填；可选 reload，默认 true。

.. warning::

   该模块占用的 ``adapters.json`` 条目会一并删除，包括你对这个条目做过的修改。重装同一个
   包不会把那些修改找回来。

POST /api/store/reinstall
-------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/reinstall``

按 catalog 重新下载并覆盖已装副本，保留适配器槽位（账号与配置）和启用状态；
catalog 版本可能没有变化。body：type + id 必填；可选 reload，默认 true。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — 模块或包未安装
   * **HTTP 500** — 下载或解压失败

GET / POST /api/store/config
----------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/store/config``

GET 返回 ``custom_url``、``disable_default`` 与 ``default_url`` （Kconfig 编进去的
内置地址）。真正生效的基址是：设了自定义地址就用它，否则用内置地址，除非
``disable_default`` 为真。POST 把两个值写入 NVS，并中止正在进行的 catalog 刷新；
非 ``http(s)`` 的地址会被拒，``disable_default`` 为真而 ``custom_url`` 为空同样是
**400** （那样商店就没有地址了）。

.. container:: mh-api-block mh-api-parameters

   **Parameters（POST JSON 请求体）**

   ``custom_url`` — 商店根 URL；空字符串清除

   ``disable_default`` — true 时不使用内置默认商店 URL

开发与量产
----------

* 开发机打包：:doc:`../plugins/pack_and_catalog`
* 工厂预装：:doc:`../production`

.. only:: not hide_l3

   * ELF ABI 不匹配时 install 失败 — 见 :doc:`module_abi`


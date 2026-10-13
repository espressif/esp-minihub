Store 插件 REST API
===================

:link_to_translation:`en:[English]`

概述
----

HTTP JSON API，用于 L1/L2 Store 模块的开发、调试、安装与运维；插件 RPC 的 HTTP 入口。

实现：components/minihub_web + minihub_store + minihub_module。
需 Kconfig：CONFIG_MINIHUB_STORE、CONFIG_MINIHUB_DYNAMIC_MODULES。

.. only:: not hide_l3

   同一套 API 也覆盖 L3 原生模块。加载它们需要
   ``CONFIG_MINIHUB_DYNAMIC_MODULES``。

Host 通用 REST 见 :doc:`host_rest`。

通用约定
--------

* 安装/升级/卸载不支持 multipart 上传 .mhub — 须先 POST /api/store/refresh，再从 catalog 安装。
* type 仅允许 module（L2）或 pack（L1）。

.. only:: not hide_l3

   ``type: module`` 也覆盖 L3。

* 默认 reload: true，成功后触发 :doc:`../architecture` 中的 Hub 软重载。


GET /api/modules
----------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules``

返回已安装 Store 模块列表及加载状态。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 数组；典型字段 id、name、version、enabled、loaded、
     runtime、adapter

   ``runtime`` 取值：

   * ``js`` — L2 QuickJS （``main.js``）

   无法分类时省略该字段。L1 pack 在 ``GET /api/packs``，不是
   ``runtime: pack``。

.. only:: not hide_l3

   ``runtime`` 也可以是 ``native`` （L3 ``.so``）或 ``hybrid`` （同时有原生与
   ``main.js`` ；可选 ``entry`` 为 ``js`` 或 ``native``）。

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
       启用或禁用；与 delete、clear_load_guard 三选一。

   ``delete`` (bool)
       true 时卸载并删除 /store/modules/ID/。

   ``clear_load_guard`` (bool)
       清除 .load_guard 后允许再次加载。

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

调用 L2 模块的 RPC（若已实现）。

.. only:: not hide_l3

   L3 模块走 ``minihub_module_rpc`` 。

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

   MCP hub.api 仅允许白名单 method 代理本接口（:doc:`mcp`）。L2 JS 通常无 native RPC。

GET /api/modules/ID/preview
---------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/modules/ID/preview``

模块可选预览资源（如摄像头帧）；响应类型因模块而异。

GET /api/packs · POST /api/packs
--------------------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/packs · POST /api/packs``

GET 返回已安装 L1 配置包列表。POST 启用/禁用或删除 L1 包（body 类似 POST /api/modules，
字段为 L1 id，无 clear_load_guard）。

GET /api/store/catalog
----------------------

.. container:: mh-api-block mh-api-command

   ``GET /api/store/catalog``

返回当前 catalog 条目。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — JSON 数组；每项含 type、id、version、url 等

POST /api/store/refresh
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/refresh``

从 GET /api/store/config 配置的 URL 重新下载 catalog.json。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 404** — URL 不可达或 catalog 缺失

POST /api/store/install
-----------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/install``

按 catalog 下载 .mhub 并解压到 /store/modules/ID/ 或 L1 包目录。

.. container:: mh-api-block mh-api-parameters

   **Parameters（JSON 请求体）**

   ``type`` (string, required)
       module 或 pack。

   ``id`` (string, required)
       catalog 中的 id。

   ``reload`` (bool, optional)
       默认 true。

   ``upgrade`` (bool, optional)
       true 时走升级路径。

.. container:: mh-api-block mh-api-returns

   **Returns**

   * **HTTP 200** — ``{"result":"ok"}``
   * **HTTP 400** — 缺少 type/id
   * **HTTP 404** — catalog 或制品不存在
   * **HTTP 409** — 已安装且未走 upgrade
   * **HTTP 500** — 下载/解压/ABI 校验失败

POST /api/store/upgrade · POST /api/store/uninstall
---------------------------------------------------

.. container:: mh-api-block mh-api-command

   ``POST /api/store/upgrade · POST /api/store/uninstall``

upgrade 用 catalog 新版本覆盖；uninstall 删除已安装条目。body：type + id 必填；可选 reload，默认 true。

GET / POST /api/store/config
----------------------------

.. container:: mh-api-block mh-api-command

   ``GET / POST /api/store/config``

GET 返回 custom_url、disable_default、当前 Store 基址。POST 写入 NVS。

.. container:: mh-api-block mh-api-parameters

   **Parameters（POST JSON 请求体）**

   ``custom_url`` — Store 根 URL；空字符串清除

   ``disable_default`` — true 时不使用内置默认 Store URL

开发与量产
----------

* 开发机打包：:doc:`../plugins/pack_and_catalog`
* 工厂预装：:doc:`../production`

.. only:: not hide_l3

   * L3 ABI 不匹配时 install 失败 — 见 :doc:`module_abi`


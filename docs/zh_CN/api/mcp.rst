MCP API
=======

:link_to_translation:`en:[English]`

概述
----

设备端 MCP 工具位于 ``components/minihub_mcp_server``，由
``CONFIG_MINIHUB_MCP_SERVER`` 打开（需要 ``components/esp-claw`` 子模块）。
本地 MCP 客户端通过产品接线的 MCP 传输访问 Hub。

工具默认在 MCP 结果中返回 JSON 文本。多数工具要求 Hub 就绪（minihub_mcp_hub_ready）；
启动或重载过程中可能返回 HUB_NOT_READY。

REST 等价接口见 :doc:`host_rest` 与 :doc:`plugin_rest`。


实现：components/minihub_mcp_server/minihub_mcp_server.c（s_tools[]）与
hub_api_gateway.c（hub.api 白名单）。

hub.api
-------

.. container:: mh-api-block mh-api-command

   ``hub.api``

白名单 REST 代理。

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``method`` (string, required)
       GET、POST 或 PUT。

   ``path`` (string, required)
       如 /api/points。

   ``query`` (string, optional)
       不含 ? 的查询串。

   ``body`` (string, optional)
       POST/PUT 的 JSON 字符串。

.. container:: mh-api-block mh-api-returns

   **Returns**

   JSON 文本，内容为对应 REST 响应体；拒绝的路径返回错误（含 /api/ota/upload 及
   非白名单模块 RPC method）。

.. container:: mh-api-block mh-api-attention

   **Attention**

   示例 — 列出点位：

.. code-block:: json

   { "method": "GET", "path": "/api/points" }

points.list
-----------

.. container:: mh-api-block mh-api-command

   ``points.list``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``type`` (string, optional)
       按点位类型过滤。

   ``adapter`` (string, optional)
       按适配器 id 过滤。

.. container:: mh-api-block mh-api-returns

   **Returns**

   点位快照 JSON 数组（``point_id``、``type``、``value``、``adapter``、
   ``online``、以及 ``props``，有则含 ``params``）。

points.get
----------

.. container:: mh-api-block mh-api-command

   ``points.get``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)
       目标点位 id。

.. container:: mh-api-block mh-api-returns

   **Returns**

   单个点位对象（字段同 ``points.list``）；参数错误或不存在时返回 INVALID_ARG / 未找到。

points.history
--------------

.. container:: mh-api-block mh-api-command

   ``points.history``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``limit`` (integer, optional)
       默认 20，最大 50。

.. container:: mh-api-block mh-api-returns

   **Returns**

   合并本地 Hub 历史与模块 device_log（若可用）的 JSON。

points.set
----------

.. container:: mh-api-block mh-api-command

   ``points.set``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``value`` (string, required)
       写入点位注册表的字符串值。

.. container:: mh-api-block mh-api-returns

   **Returns**

   成功时确认 JSON；对可电源控制类型，on/off/true/false/1/0 亦映射为 turn_on / turn_off
   （经 command.invoke）。

command.invoke
--------------

.. container:: mh-api-block mh-api-command

   ``command.invoke``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``cmd`` (string, required)

   ``params_json`` (string, optional)
       适配器参数的 JSON 对象字符串。目录设备请带 ``parameter``，以便更新
       该设备点上的 ``props.params``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   等价 POST /api/command 的结果 JSON。

rules.list
----------

.. container:: mh-api-block mh-api-command

   ``rules.list``

无参数。

.. container:: mh-api-block mh-api-returns

   **Returns**

   当前 rules.json 内容 JSON。

rules.put
---------

.. container:: mh-api-block mh-api-command

   ``rules.put``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``rules_json`` (string, required)
       完整规则文档字符串。

.. container:: mh-api-block mh-api-returns

   **Returns**

   替换规则并热重载规则引擎后的确认 JSON。

adapters.get
------------

.. container:: mh-api-block mh-api-command

   ``adapters.get``

无参数。

.. container:: mh-api-block mh-api-returns

   **Returns**

   只读 adapters.json 片段 JSON。

hub.reload
----------

.. container:: mh-api-block mh-api-command

   ``hub.reload``

无参数；调用 minihub_hub_reload。

.. container:: mh-api-block mh-api-returns

   **Returns**

   确认 JSON。

.. container:: mh-api-block mh-api-attention

   **Attention**

   限速每 5 秒一次。

store.config.get
----------------

.. container:: mh-api-block mh-api-command

   ``store.config.get``

无参数。

.. container:: mh-api-block mh-api-returns

   **Returns**

   NVS 中 custom_url 与 disable_default JSON。

store.config.set
----------------

.. container:: mh-api-block mh-api-command

   ``store.config.set``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``custom_url`` (string)
       http(s)://host:port；空字符串清除覆盖。

   ``disable_default`` (boolean)
       true 跳过内置 Store URL。

.. container:: mh-api-block mh-api-returns

   **Returns**

   确认 JSON。

hub.api 白名单
--------------

网关规则见 hub_api_gateway.c（s_rules[]）：

**GET**

* /api/、/api/system、/api/update/notes、/api/points、/api/points/history
* /api/rules、/api/adapters、/api/config/adapters
* /api/location、/api/modules、/api/packs、/api/scenes、/api/events
* /api/store/catalog、/api/store/config、/api/ota
* /api/matter/status（Matter 构建）
* /api/thread/status、/api/thread/dataset（Thread 构建）
* /api/radio/provision、/api/zigbee（Zigbee 构建）
* /api/modules/ID/preview（路径中的星号仅作通配符）

**POST 方法**

* /api/command、/api/rules/import、/api/scenes/import、/api/scenes/invoke
* /api/store/config、/api/store/install、/api/store/upgrade、/api/store/uninstall、/api/store/refresh
* /api/hub/reload、/api/points/remove、/api/ota
* /api/thread/start、/api/thread/stop（Thread 构建）
* Matter / Zigbee / BLE Mesh / Thread 构建相关路径
* /api/modules/ID/rpc — 仅当 RPC method 在白名单内

**PUT 方法**

* /api/config/adapters

模块 RPC 白名单 (s_rpc_allow[])
--------------------------------

status、homes、devices、device_log、prop_get、prop_probe、camera_preview、sync、
logout、login、session、verify、verify_send、login_retry

其他 RPC 方法须直接走局域网 REST，不可经 hub.api。


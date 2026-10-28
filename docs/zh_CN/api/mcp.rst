MCP API
=======

:link_to_translation:`en:[English]`

概述
----

Hub 可以把自己变成一个 MCP 服务。助手便可操作一台已经烧好的设备。工具位于
``components/minihub_mcp_server``，由 ``CONFIG_MINIHUB_MCP_SERVER`` 打开（需要
``components/esp-claw`` 子模块）。本地 MCP 客户端经产品接线的传输访问 Hub。

工具一律在 MCP 结果里返回 JSON 文本。多数工具要求 Hub 已就绪（minihub_mcp_hub_ready），
所以在启动或重载过程中调用可能返回 HUB_NOT_READY。

REST 等价接口见 :doc:`host_rest` 与 :doc:`plugin_rest`；工具可能返回的每个 ``code``
见 :doc:`errors`。


实现：components/minihub_mcp_server/minihub_mcp_server.c（s_tools[]）与
hub_api_gateway.c（hub.api 白名单）。

hub.api
-------

.. container:: mh-api-block mh-api-command

   ``hub.api``

白名单 REST 式访问。网关在进程内执行调用（``hub_api_call``），不转发给
``minihub_web`` 的 HTTP 服务，因此没有局域网客户端也能用，并直接返回 JSON。

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

   响应 JSON 文本。被拒路径回 ``DENIED``；在白名单内但网关没有本地处理分支的
   路径回 ``NOT_IMPLEMENTED``；模块 RPC 的 ``method`` 不在白名单时拒绝执行。

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

   ``{"ok":true,"points":[…]}``；每个快照含 ``point_id``、``type``、
   ``value``、``adapter``、``online``，有则含 ``params`` 的 ``props``。

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

   ``{"ok":true,"point":{…}}``，字段同 ``points.list``；参数错误或不存在时返回
   ``INVALID_ARG`` / ``NOT_FOUND``。

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

   ``{"ok":true,"point_id":…,"items":[…]}``，新的在前。每条含 ``ts_ms``、
   ``old``、``new``、``label`` 与 ``source`` （``local`` 或 ``cloud``）。本地 Hub
   环形缓冲会与所属适配器提供的 ``device_log`` 合并；云端条目与本地条目同
   ``label`` 且时间相差 2 秒以内时视为重复并丢弃；取日志失败的原因写入
   ``cloud_error``。

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

   ``{"ok":true}``。对可电源控制的类型，on/off/true/false/1/0 经适配器执行
   turn_on / turn_off；其他值只写入点位注册表。

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

   ``{"ok":true}``；适配器拒绝命令时为 ``INVOKE_FAILED``。

rules.list
----------

.. container:: mh-api-block mh-api-command

   ``rules.list``

无参数。

.. container:: mh-api-block mh-api-returns

   **Returns**

   当前 rules.json 对象，另加 ``"ok":true``。

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

   规则引擎接受并保存后的 ``{"ok":true,"count":N}``；不接受时回
   ``VALIDATION_FAILED`` / ``RELOAD_FAILED`` / ``SAVE_FAILED``。

adapters.get
------------

.. container:: mh-api-block mh-api-command

   ``adapters.get``

无参数。

.. container:: mh-api-block mh-api-returns

   **Returns**

   只读 adapters.json 对象，另加 ``"ok":true``。

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

   ``{"ok":true,"custom_url":…,"disable_default":…,"default_url":…}``。

store.config.set
----------------

.. container:: mh-api-block mh-api-command

   ``store.config.set``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``custom_url`` (string)
       http(s)://host:port；空字符串清除覆盖。

   ``disable_default`` (boolean)
       true 跳过内置商店 URL。

.. container:: mh-api-block mh-api-returns

   **Returns**

   与 ``store.config.get`` 相同。

hub.api 白名单
--------------

网关规则见 hub_api_gateway.c（s_rules[]）。路径先过白名单，再需要对应的本地
处理分支；没有分支时即使列在白名单里也回 ``NOT_IMPLEMENTED``。

已实现的接口并不是 REST 应答的原样拷贝：网关用同样的核心调用自己拼 JSON。
这里的 ``GET /api/ota`` 在 ``state`` / ``error`` / ``progress`` /
``bytes_written`` / ``source`` 之外还多给 ``ok`` 与 ``busy`` （见
:doc:`host_rest`）；而读取类调用拿不到 REST 路由本会返回的 HTTP 状态码 ——
被拒绝时，MCP 结果本身是成功的，里面是一个 ``error`` 对象。

**GET — 可用**

* ``/api/system``、``/api/points``、``/api/points/history``、``/api/ota``
* ``/api/rules``、``/api/adapters``、``/api/config/adapters``、``/api/modules``
* ``/api/store/catalog``、``/api/store/config``、``/api/radio/provision``
* ``/api/matter/status`` （Matter 构建）
* ``/api/zigbee`` （Zigbee 构建）—— 由网关直接组装的 Zigbee 摘要；Host HTTP
  服务没有对应路由

**POST — 可用**

* ``/api/command``、``/api/rules/import``、``/api/hub/reload``、
  ``/api/points/remove``、``/api/ota``
* ``/api/store/config``、``/api/store/install``、``/api/store/upgrade``、
  ``/api/store/uninstall``、``/api/store/refresh``
* ``/api/matter/commission``、``/api/matter/devices`` （Matter 构建）
* ``/api/zigbee/permit_join`` （Zigbee 构建）
* ``/api/ble_mesh/permit_provision`` （BLE Mesh 构建）
* ``/api/modules/ID/rpc`` —— 仅当 RPC ``method`` 在白名单内

.. warning::

   这份清单里的写操作意味着：一个 MCP 客户端是 **操作者，不是观察者** 。它可以用
   ``hub.api`` 整份替换规则文档、删除点位、卸载商店包、发起 OTA、重载 Hub。
   ``CONFIG_MINIHUB_MCP_SERVER`` 只有一个总开关，没有只读模式；白名单是编译期的表
   （ ``s_rules[]``），没有可按客户端配置的东西：拿到 token 的人就拿到了全部。只把
   这个服务器开给你放心让他重配整屋设备的智能体。

**PUT — 可用**

* ``/api/config/adapters``

**在白名单内、无本地处理**

这些路径回 ``NOT_IMPLEMENTED``，请直接走局域网 REST（:doc:`host_rest`）：

* GET：``/api/``、``/api/update/notes``、``/api/location``、``/api/packs``、
  ``/api/scenes``、``/api/events``、``/api/modules/ID/preview``
* GET：``/api/thread/status``、``/api/thread/dataset`` （Thread 构建）
* POST：``/api/scenes/import``、``/api/scenes/invoke``
* POST：``/api/thread/start``、``/api/thread/stop``、``/api/thread/dataset``
  （Thread 构建）

模块 RPC 白名单（``s_rpc_allow[]``）
------------------------------------

``status``、``homes``、``devices``、``device_log``、``prop_get``、``prop_probe``、
``camera_preview``、``sync``、``logout``、``login``、``session``、``verify``、
``verify_send``、``login_retry``

其他 RPC 方法须直接走局域网 REST，不可经 ``hub.api``。


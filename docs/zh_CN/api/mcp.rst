MCP API
=======

:link_to_translation:`en:[English]`

概述
----

已烧录的 Hub 可在局域网上提供 MCP 服务，供助手读写点位并调用白名单内的 Host
REST。``CONFIG_MINIHUB_MCP_SERVER`` 默认打开；实现位于
``components/minihub_mcp_server``，HTTP 传输来自 ``components/esp-claw`` 子模块。

传输为 HTTP，默认监听 ``18791``、路径 ``/mcp``。Wi-Fi STA、以太网或配网 SoftAP
任一接口获得 IPv4 后开始监听。局域网发现使用 mDNS：服务类型 ``_mcp._tcp``，
主机名 ``esp-minihub``，实例名 ``ESP-MiniHub``。

工具结果均为 JSON 文本。多数工具要求 Hub 已就绪；启动或 ``hub.reload`` 过程中
调用可能返回 ``HUB_NOT_READY``。

REST 等价接口见 :doc:`host_rest` 与 :doc:`plugin_rest`。工具返回的 ``code`` 见
:doc:`errors`。

怎么使用
--------

编译与烧录
~~~~~~~~~~

1. ``git submodule update --init components/esp-claw``
2. ``CONFIG_MINIHUB_MCP_SERVER`` 默认打开（``y``）
3. 烧录后串口出现 ``MCP server ready: http://esp-minihub.local:18791/mcp``

地址
~~~~

* STA / 以太网：``http://<Hub_IPv4>:18791/mcp`` 或 ``http://esp-minihub.local:18791/mcp``
* SoftAP（SSID ``PROV_MiniHub``）：``http://192.168.4.1:18791/mcp``
* mDNS：类型 ``_mcp._tcp``，实例 ``ESP-MiniHub``，主机 ``esp-minihub.local``

客户端
~~~~~~

任一支持 streamable HTTP 的 MCP 客户端。配置示例::

   {
     "mcpServers": {
       "minihub": {
         "type": "streamable-http",
         "url": "http://HUB_IP:18791/mcp"
       }
     }
   }

调用
~~~~

1. ``hub.routes``
2. ``points.list`` （未用的 ``type`` / ``adapter`` 传 ``""``）
3. ``command.invoke`` 或 ``points.set``
4. ``hub.api`` —— ``method`` / ``path`` / ``query`` / ``body`` 每次都要带::

      {
        "method": "GET",
        "path": "/api/system",
        "query": "",
        "body": ""
      }

   常用 GET：``/api/system``、``/api/points``、``/api/store/catalog``、
   ``/api/store/config``、``/api/ota``。

hub.api
-------

.. container:: mh-api-block mh-api-command

   ``hub.api``

白名单 REST 式访问。网关在进程内执行调用（``hub_api_call``），不转发给
``minihub_web`` 的 HTTP 服务，因此没有局域网客户端也能用，并直接返回 JSON。

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``method`` (string, required)
       ``GET``、``POST`` 或 ``PUT``。

   ``path`` (string, required)
       白名单路径，如 ``/api/points`` 或 ``/api/system``。
       ``/api/status``、``/api/version``、``/api/info`` 不在白名单。

   ``query`` (string, 每次必传)
       不含 ``?`` 的查询串。不用时传 ``""`` —— MCP SDK 要求已注册属性每次都出现。

   ``body`` (string, 每次必传)
       POST/PUT 的 JSON 字符串。GET 时传 ``""``。

.. container:: mh-api-block mh-api-returns

   **Returns**

   响应 JSON 文本。被拒路径回 ``DENIED``（含 ``hint`` 与 ``try`` 起步路径）。
   完整白名单见 ``hub.routes``。白名单内但无本地处理分支回 ``NOT_IMPLEMENTED``；
   模块 RPC 的 ``method`` 不在白名单时拒绝执行。

.. container:: mh-api-block mh-api-attention

   **Attention**

   示例 — 查 Hub 状态与列点位：

.. code-block:: JSON

   { "method": "GET", "path": "/api/system", "query": "", "body": "" }

.. code-block:: JSON

   { "method": "GET", "path": "/api/points", "query": "", "body": "" }

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

   ``{"ok":true}``。点位 ``props.params`` 含 ``power`` 时，
   ``on``/``off``/``true``/``false``/``1``/``0`` 经适配器执行
   ``turn_on`` / ``turn_off``；其他值只写入点位注册表。

command.invoke
--------------

.. container:: mh-api-block mh-api-command

   ``command.invoke``

.. container:: mh-api-block mh-api-parameters

   **Parameters**

   ``point_id`` (string, required)

   ``cmd`` (string, required)

   ``params_json`` (string, optional)
       适配器参数的 JSON 对象字符串。目录设备带 ``parameter`` 时更新该设备点上
       的 ``props.params``。

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

   当前 ``rules.json`` 对象，另加 ``"ok":true``。

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

   只读 ``adapters.json`` 对象，另加 ``"ok":true``。

hub.routes
----------

.. container:: mh-api-block mh-api-command

   ``hub.routes``

无参数。返回编译期 ``hub.api`` 白名单（method + path，来自 ``s_rules[]``）。

.. container:: mh-api-block mh-api-returns

   **Returns**

   ``{"ok":true,"note":…,"routes":[{"method":"GET","path":"/api/system"},…]}``。

hub.reload
----------

.. container:: mh-api-block mh-api-command

   ``hub.reload``

无参数；调用 ``minihub_hub_reload()``。

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
       ``http(s)://host:port``；空字符串清除覆盖。

   ``disable_default`` (boolean)
       ``true`` 跳过内置商店 URL。

.. container:: mh-api-block mh-api-returns

   **Returns**

   与 ``store.config.get`` 相同。

hub.api 白名单
--------------

网关规则见 ``hub_api_gateway.c`` （ ``s_rules[]`` ）。路径先过白名单，再需要对应的本地
处理分支；没有分支时即使列在白名单里也回 ``NOT_IMPLEMENTED``。

已实现的接口并不是 REST 应答的原样拷贝：网关用同样的核心调用自己拼 JSON。
``GET /api/system`` 回 ``ok``、``name``、``version``、``idf_version``、``commit``、
``uptime_s``、``chip``、``local_ip``、``web_base`` （Host REST
:doc:`host_rest` 的子集，不含 RAM / 存储 / 网络细节）。
``GET /api/update/notes`` 与 Host REST 同形
（``firmware_version`` / ``firmware_note`` / ``web_version`` / ``web_note``）。
这里的 ``GET /api/ota`` 在 ``state`` / ``error`` / ``progress`` /
``bytes_written`` / ``source`` 之外还多给 ``ok`` 与 ``busy``；读取类调用拿不到
REST 路由本会返回的 HTTP 状态码 —— 被拒绝时，MCP 结果本身是成功的，里面是一个
``error`` 对象。

**GET — 可用**

* ``/api/system``、``/api/update/notes``、``/api/points``、``/api/points/history``、
  ``/api/ota``
* ``/api/rules``、``/api/scenes``、``/api/adapters``、``/api/config/adapters``、
  ``/api/modules``
* ``/api/store/catalog``、``/api/store/config``、``/api/radio/provision``
* ``/api/matter/status`` （Matter 构建）
* ``/api/zigbee`` （Zigbee 构建）—— 由网关直接组装的 Zigbee 摘要；Host HTTP
  服务没有对应路由

.. warning::

   写操作经 ``hub.api`` 可替换规则、删除点位、卸载商店包、发起 OTA、重载 Hub、
   重启设备。``CONFIG_MINIHUB_MCP_SERVER`` 只有总开关，无只读模式；白名单为编译期
   ``s_rules[]``，不可按客户端裁剪。

**POST — 可用**

* ``/api/command``、``/api/rules/import``、``/api/scenes/import``、
  ``/api/scenes/invoke``、``/api/hub/reload``、``/api/system/reboot``
  （body ``{"confirm":true}``）、``/api/points/remove``、``/api/ota``
* ``/api/store/config``、``/api/store/install``、``/api/store/upgrade``、
  ``/api/store/uninstall``、``/api/store/refresh``
* ``/api/matter/commission``、``/api/matter/devices`` （Matter 构建）
* ``/api/zigbee/permit_join`` （Zigbee 构建）
* ``/api/ble_mesh/permit_provision`` （BLE Mesh 构建）
* ``/api/modules/*/rpc`` —— 仅当 RPC ``method`` 在白名单内

**PUT — 可用**

* ``/api/config/adapters``

**在白名单内、无本地处理**

这些路径回 ``NOT_IMPLEMENTED``。局域网 REST：:doc:`host_rest`。

* GET：``/api/``、``/api/location``、``/api/packs``、``/api/events``、
  ``/api/modules/*/preview``
* GET：``/api/thread/status``、``/api/thread/dataset`` （Thread 构建）
* POST：``/api/thread/start``、``/api/thread/stop``、``/api/thread/dataset``
  （Thread 构建）

模块 RPC 白名单（``s_rpc_allow[]``）
------------------------------------

``status``、``homes``、``devices``、``device_log``、``prop_get``、``prop_probe``、
``camera_preview``、``sync``、``logout``、``login``、``session``、``verify``、
``verify_send``、``login_retry``

其他 RPC 方法不在白名单；局域网 REST：:doc:`host_rest`。


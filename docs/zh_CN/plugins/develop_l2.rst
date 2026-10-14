开发 L2（JS）模块
=================

:link_to_translation:`en:[English]`

L2 模块在 Hub 上运行 **QuickJS** 脚本。Host 在适配器启动后及软重载
（``POST /api/hub/reload`` ）时加载 ``main.js``。实现见
``components/minihub_js/minihub_js.c``。

适用场景
--------

* 轻量自动化、HTTP 轮询、REST 与点位之间的胶水逻辑
* 通过 ``/modules/`` 提供 UI 片段，无需原生代码

.. only:: not hide_l3

   * 在投入 L3 之前快速验证想法

要求：产品 sdkconfig 打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` （``app.yml`` 中
``dynamic_modules`` 特性）。

目录结构
--------

.. code-block:: text

   examples/plugins/ID/
     module.json
     main.js              # 必需 — 须导出 setup(host)
     meta.json            # 可选 — ui + capabilities
     web/                 # 可选静态资源

``module.json`` 示例
--------------------

.. code-block:: json

   {
     "id": "demo_js",
     "name": "Demo JS 模块",
     "version": "0.1.0",
     "enabled": true,
     "runtime": "js",
     "adapter": "demo_js",
     "abi_version": 1,
     "core_api_min": 1,
     "core_api_max": 1,
     "capabilities": ["points"]
   }

若省略 ``runtime``，且存在 ``main.js``、无 ``CMakeLists.txt``，Host 按 JS 模块处理。

``main.js`` 约定
----------------

Host 每次加载调用 ``setup(host)``。``host`` 对象提供：

* ``host.log.info/warn/error(msg)``
* ``host.point.register({ point_id, type, adapter, value, online, props|params })`` —
  一台设备一条点（``协议.设备``）。``type`` 须是类型目录里的 ``device_id``
  （如 ``light`` / ``switch``），设备页才有标准控件；见 :doc:`../type_catalog`。
  传入 ``params``（简写）或完整 ``props``，使 ``props.params`` 到达 UI。
* ``host.point.get(point_id)`` · ``host.point.set(point_id, value, props?)`` ·
  ``host.point.unregister(point_id)``
* ``host.event.subscribe(event_type, handler)`` — ``point.updated`` /
  ``point.registered`` （在同一 Host ``point_id`` 上补充字段）
* ``host.command.on(adapter, handler)`` — 在 JS 中接收 UI/REST 命令
* ``host.command.invoke({ point_id, cmd, params? })``
* ``host.http.request({ url, method, body, timeout_ms })``
* ``host.config.get()``

厂商插件丰富 Zigbee（或其他 Host）设备时必须用**同一** ``point_id``。
扩展写在顶层 ``props``：``manufacturer`` / ``category`` / ``ota`` /
``owner`` 是身份与分组元数据，不是类型目录参数。适配器遥测只合并
``props.params``，因此这些扩展会保留。自建点应在 ``register`` 时带上厂商
与品类（演示 ``color_bulb_js``）。完整 Host API：:doc:`../api/js_host`。

设备页演示（三盏灯）：``examples/plugins/color_bulb_js/``。

最小 ``main.js`` ：

.. code-block:: javascript

   function setup(host) {
     host.log.info("demo_js starting");
     host.point.register({
       point_id: "demo_js.hello",
       type: "sensor",
       adapter: "demo_js",
       value: "ok",
       online: true
     });
   }

可选 ``ui.json`` 描述 Web UI 中的模块表单（完整示例见
``examples/plugins/color_bulb_js/`` ）。

接入 ``adapters.json``
----------------------

增加适配器块（``devices`` 可为空）：

.. code-block:: json

   {
     "adapters": {
       "demo_js": { "enabled": true, "devices": [] }
     }
   }

打包、部署、测试
----------------

.. code-block:: bash

   python3 tools/pack_store_module.py pack demo_js
   python3 tools/sync_store_catalog.py --from-store
   python3 tools/store_server.py
   # Hub: POST /api/store/config {"custom_url":"http://YOUR_PC:8080"}
   curl -X POST http://$HUB/api/store/refresh
   curl -X POST http://$HUB/api/store/install \
     -H 'Content-Type: application/json' \
     -d '{"type":"module","id":"demo_js"}'
   curl -X POST http://$HUB/api/hub/reload

验证：

* ``GET /api/modules`` — ``demo_js`` 已列出且 ``loaded: true``
* ``GET /api/points`` — 可见 ``demo_js.hello``
* 串口：``JS module 'demo_js' setup ok``

L2 可选模块 RPC；更推荐 ``host.command`` 或 REST。API 详解 :doc:`../api/js_host`。

.. only:: not hide_l3

   L3 RPC 见 :doc:`develop_l3` 。

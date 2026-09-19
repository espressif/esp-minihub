L2 JS Host API
==============

:link_to_translation:`en:[English]`

概述
----

Store **L2** 模块（``runtime: js``，``main.js``）通过 QuickJS 绑定访问 Host 能力。
实现位于 ``components/minihub_js/minihub_js.c``；需要 ``CONFIG_MINIHUB_JS_PLUGINS``。

REST 安装与 RPC 见 :doc:`plugin_rest`；C 侧点位/命令模型见 :doc:`module_abi` 与
:doc:`minihub_core`。

模块入口
--------

Hub 在模块加载及每次 ``POST /api/hub/reload`` 后对 enabled 的 JS 模块调用全局函数
``setup(host)``。``main.js`` 必须定义 ``setup``；缺失则打 warning 并跳过模块。

在 ``setup`` 中为模块 id 使用独立 ``point_id`` 前缀；重载时会再次调用，避免重复注册。
**一台设备登记一条点** （``模块.设备``），不要按参数再拆点。

实现
----

* ``components/minihub_js/minihub_js.c`` — QuickJS 绑定

入口函数
--------

setup(host)
~~~~~~~~~~~

.. code-block:: javascript

   function setup(host) { ... }

Hub 注入的能力对象；见下文各 ``host.*`` 字段。

Parameters
^^^^^^^^^^

``host`` (object)
    Host 注入的能力对象。

Returns
^^^^^^^

``undefined``。

方法 — host.log
---------------

host.log.info / warn / error
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: javascript

   host.log.info(msg)
   host.log.warn(msg)
   host.log.error(msg)

写 ESP-IDF 日志，tag ``minihub_js``。

Parameters
^^^^^^^^^^

``msg`` (string)
    日志消息。

Returns
^^^^^^^

``undefined``。

方法 — host.point
-----------------

host.point.register(obj)
~~~~~~~~~~~~~~~~~~~~~~~~

在 Hub 点位注册表注册或更新一个点位。

Parameters
^^^^^^^^^^

``obj.point_id`` (string, required)
    最长 64 字符。

``obj.type`` (string, optional)
    默认 ``sensor``。

``obj.adapter`` (string, optional)
    默认 ``js``；建议与 ``module.json`` 的 ``adapter`` 一致。

``obj.value`` (string, optional)
    初始值。

``obj.online`` (boolean, optional)
    默认 false。

``obj.props`` (object, optional)
    完整 props JSON，Core 会复制。设备页控件常用
    ``props.params.<name> = { value, access?, visible?, min?, max?, … }``。
    厂商 / 品类写顶层：``manufacturer``、``category`` （见 :doc:`../data_model`）。

``obj.params`` (object, optional)
    简写：未传 ``props`` 时 Host 包装为 ``{ "params": <obj> }``。

Returns
^^^^^^^

``undefined``；失败**抛错** （QuickJS exception）。

host.point.get(point_id)
~~~~~~~~~~~~~~~~~~~~~~~~

读取点位快照。

Returns
^^^^^^^

对象，字段含 ``point_id``、``type``、``value``、``adapter``、``online``、``props``；
不存在时为 ``null``。

host.point.set(point_id, value, props?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

更新 ``value``（及可选 ``props``）并发布 ``point.updated``。

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    目标点位。

``value`` (string, required)
    摘要值（如 ``on`` / ``off``）。

``props`` (object, optional)
    若提供则**整对象替换**点位 props。请用 ``host.point.get`` 再 ``set`` 做
    读-改-写，或把扩展放在顶层键（``ota``、``ieee`` 等），这样适配器遥测只
    合并 ``props.params`` 时不会冲掉它们。

Returns
^^^^^^^

``undefined``。

host.point.mergeProps(point_id, props_patch, value?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

把 ``props_patch`` 的顶层键浅合并进活动点位（``minihub_point_merge_props``）。
用于写入身份 / ``ota``，而不冲掉 ``props.params``。

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    目标点位。

``props_patch`` (object, required)
    要合并的顶层键（如 ``{ manufacturer: "Espressif", category: "Color Bulb", ota: { available: true } }``）。

``value`` (string, optional)
    新摘要值；省略则保持原摘要。

Returns
^^^^^^^

``undefined``；失败则 **抛出**。

host.point.mergeParams(point_id, params, value?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

只替换 ``props.params``，保留其他顶层键（``minihub_point_merge_params``）。
遥测更新优先用此接口。

Returns
^^^^^^^

``undefined``；失败则 **抛出**。

方法 — host.event
-----------------

host.event.subscribe(event_type, handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

订阅 Core 事件总线（封装 ``minihub_event_bus_subscribe``）。``event_type`` 可为
具体名称或 ``*`` （全部事件）。Host 名称与载荷见 :doc:`host_rest`
（``GET /ws/events``）。常见例子：``point.updated``、``point.registered``。

Parameters
^^^^^^^^^^

``event_type`` (string, required)
    事件名，或 ``*`` 表示全部。

``handler`` (function, required)
    调用形式 ``handler(evt)``。``point.updated`` 时含 ``point_id``、``value``、
    ``props``、``old_value``、``new_value``；``point.registered`` 时
    ``evt.data`` 含 ``point_id``。

Returns
^^^^^^^

``undefined``。

.. code-block:: javascript

   host.event.subscribe("point.updated", function (evt) {
     if (evt.point_id === "zigbee.zb_0a1b") {
       host.point.mergeProps(evt.point_id, {
         ota: { available: true, version: "1.2.3", url: "https://…" }
       });
     }
   });

方法 — host.command
-------------------

host.command.on(adapter, handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

为 ``adapter`` 注册 JS 命令处理函数（应与 ``module.json`` / 点位的 ``adapter``
一致）。Core ``minihub_command_invoke`` 将 UI/REST 命令路由到 ``handler``。

Parameters
^^^^^^^^^^

``adapter`` (string, required)
    适配器名。

``handler`` (function, required)
    调用形式 ``handler({ point_id, cmd, params })``。``params`` 为对象或
    ``null``。返回 ``false`` 表示失败；抛错 → Core ``ESP_FAIL``。

Returns
^^^^^^^

``undefined``。

host.command.invoke({ point_id, cmd, params? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

经 ``minihub_command_invoke`` 路由，同 :doc:`host_rest` 的 ``POST /api/command``。

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    目标点位。

``cmd`` (string, required)
    命令名。

``params`` (object, optional)
    JSON 参数（设备控制常用 ``{ parameter, value }``）。

Returns
^^^^^^^

``undefined``。

See also
^^^^^^^^

三盏可控灯演示：``examples/plugins/color_bulb_js/``。

方法 — host.http
----------------

host.http.request(opts)
~~~~~~~~~~~~~~~~~~~~~~~

经 ``esp_http_client`` 发起 HTTP(S) 请求。

Parameters
^^^^^^^^^^

``opts.url`` (string, required)
    完整 URL；``https://`` 走 TLS。

``opts.method`` (string, optional)
    ``GET`` （默认）、``POST``、``PUT``。

``opts.body`` (string, optional)
    POST/PUT 正文。

``opts.timeout_ms`` (number, optional)
    默认 15000。

Returns
^^^^^^^

``{ status: number, body: string }`` — ``status`` 为 HTTP 状态码。

Attention
^^^^^^^^^

响应体上限 **16384** 字节；``main.js`` 最大 **256 KiB**。出站 URL 须符合 Host 网络安全策略。

方法 — host.config
------------------

host.config.get()
~~~~~~~~~~~~~~~~~

只读返回当前 ``adapters.json`` 根对象。

Returns
^^^^^^^

JavaScript 对象，或 ``null``。

Attention
^^^^^^^^^

JS 侧不能写回配置；持久化请用 REST PUT /api/config/adapters 或 L3 虚表
config_save_adapters（:doc:`module_abi`）。

见 :doc:`../plugins/develop_l2`。

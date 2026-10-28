L2 JS Host API
==============

:link_to_translation:`en:[English]`

概述
----

L2 模块是跑在 Hub 里的 JavaScript，其 ``module.json`` 以 ``runtime: js`` 标明层级。
它通过 QuickJS 绑定访问 Host 能力，不靠链接器。这些绑定实现在
``components/minihub_js/minihub_js.c``，由
``CONFIG_MINIHUB_JS_PLUGINS`` 决定是否编入。

REST 安装与 RPC 见 :doc:`plugin_rest`。C 侧点位/命令模型见 :doc:`minihub_core`。

host 对象
---------

``setup(host)`` 拿到的就是下表，构建时直接从 QuickJS 绑定里读出来，所以改名或删掉的
方法不会继续留在本页。斜杠后的数字是 C 侧登记的参数个数；表按命名空间列出各自的方法。

.. minihub-js-api::

.. only:: not hide_l3

   L3 使用 :doc:`module_abi` 中的虚表。

模块入口
--------

Hub 在模块加载及每次 ``POST /api/hub/reload`` 后对 enabled 的 JS 模块调用
``setup(host)``。``main.js`` 必须定义 ``setup``；缺失则打 warning 并跳过模块。
每个模块的顶层名字只留在这个模块里。

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
    完整 props JSON，Core 会复制。概览页控件常用
    ``props.params.<name> = { value, access?, visible?, min?, max?, … }``。
    厂商 / 品类写顶层：``manufacturer``、``category`` （见 :doc:`../data_model`）。

``obj.params`` (object, optional)
    简写：未传 ``props`` 时 Host 包装为 ``{ "params": <obj> }``。

Returns
^^^^^^^

``undefined``；失败 **抛错** （QuickJS exception）。

host.point.get(point_id)
~~~~~~~~~~~~~~~~~~~~~~~~

读取点位快照。

Returns
^^^^^^^

对象，字段含 ``point_id``、``type``、``value``、``adapter``、``online``、``props``；
不存在时为 ``null``。

host.point.set(point_id, value, props?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

更新 ``value`` （及可选 ``props``）并发布 ``point.updated``。

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    目标点位。

``value`` (string, required)
    摘要值（如 ``on`` / ``off``）。

``props`` (object, optional)
    若提供则 **整对象替换** 点位 props。请用 ``host.point.get`` 再 ``set`` 做
    读-改-写，或把扩展放在顶层键（``ota``、``ieee`` 等），这样适配器遥测只
    合并 ``props.params`` 时不会冲掉它们。

Returns
^^^^^^^

``undefined``。

host.point.unregister(point_id)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

删除一个点位并发布 ``point.unregistered``。点位不存在时不报错。

Parameters
^^^^^^^^^^

``point_id`` (string, required)
    目标点位。

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

方法 — host.time
-----------------

系统页选择的时区是固定偏移（``UTC``、``UTC+8``、``UTC+5:30``），保存在 NVS，
默认 UTC。``offsetMin`` 是相对 UTC 向东的分钟数，不含夏令时。

host.time.offsetMin() / host.time.id()
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: javascript

   var minutes = host.time.offsetMin();
   var id = host.time.id();

Returns
^^^^^^^

``offsetMin`` 返回整数。``id`` 返回字符串，例如 ``UTC+8``。

方法 — host.timer
-----------------

定时器在 Host worker 任务上运行 JS 回调（QuickJS 为单线程）。这是 JS 模块做
周期/延时任务的唯一手段，适合轮询或去抖。同样这四个函数也注入为全局
（``setTimeout`` / ``clearTimeout`` / ``setInterval`` / ``clearInterval``）。

host.timer.setTimeout(fn, ms) / host.timer.setInterval(fn, ms)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: javascript

   var id = host.timer.setTimeout(function () { host.log.info("只触发一次"); }, 1000);
   var poll = host.timer.setInterval(function () { /* 读传感器 */ }, 5000);
   host.timer.clearTimeout(id);
   host.timer.clearInterval(poll);

Parameters
^^^^^^^^^^

``fn`` （function，必填）
    无参回调，运行在 JS worker 任务上。

``ms`` （number，必填）
    延时毫秒（``setInterval`` 为周期）。``<= 0`` 按 ``1`` ms 处理。
    参数顺序 ``(fn, ms)`` 或 ``(ms, fn)`` 均可。

Returns
^^^^^^^

整数 id（从 1 起）。每个模块最多 16 个存活定时器，超出 **抛异常**。

说明
^^^^

* ``setup`` 内可启动定时器；在 ``clear*`` 或重载前持续有效。
* 长时间运行的回调会阻塞其它定时器与任务——保持回调简短。
* ``POST /api/hub/reload`` 会清除所有定时器并再次运行 ``setup``。

方法 — host.adapter
-------------------

.. code-block:: javascript

   host.adapter.poll(function () { /* 从云端刷新点位 */ });
   host.adapter.clearPoll();

host.adapter.poll(fn)
~~~~~~~~~~~~~~~~~~~~~

在 Host 适配器轮询节拍下运行 ``fn``，约每 5 秒一次。节拍只负责唤醒 JS worker，
因此轮询不会拖慢适配器轮询或 hub 重载；回调本身跑在 JS worker 任务上
（QuickJS 单线程）。

参数
^^^^

``fn`` （函数，必填）
    无参回调。传入其它类型抛 ``TypeError``。

返回
^^^^

``undefined``。整个运行时最多容纳 8 个轮询回调，第 9 个抛异常。回调抛出的异常
只记录后被丢弃，节拍照常继续。

host.adapter.clearPoll()
~~~~~~~~~~~~~~~~~~~~~~~~

清除该 JS 运行时里 **所有** 轮询回调，包括别的 L2 模块注册的。没有按回调取消的
接口：某个模块想停止轮询，就自己置一个标志位在回调里提前返回。

说明
^^^^

* 上一批回调还在执行时本次节拍直接跳过，回调不会堆积。
* 重载（``POST /api/hub/reload`` ）会清空所有回调并重新运行 ``setup``。
* 按框架节奏做云端或局域网刷新用它；需要别的周期用 ``host.timer.setInterval``。

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

``{ status: number, body: string, headers?: { location?: string, "set-cookie"?: string, "extension-pragma"?: string } }`` — ``status`` 为 HTTP 状态码。

Attention
^^^^^^^^^

响应体上限 **65536** 字节；``main.js`` 最大 **1 MiB**。出站 URL 须符合 Host 网络安全策略。
可选 ``headers`` 对象设置请求头（如 ``Cookie``、``Content-Type``）。自动重定向 **关闭**，便于读取
``Location`` / ``Set-Cookie`` （护照登录）。除 GET/POST/PUT 外支持 ``DELETE``。

host.http.requestAsync(opts)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``opts`` 与 ``host.http.request`` 相同，但返回 **Promise**，并把阻塞的 socket 收发放到独立任务上，
不再占用 JS worker。多请求的链路（云端属性批量刷新）应使用它：云请求进行中时，设备控制、
同步状态机与 UI 仍可响应。

Returns
^^^^^^^

``Promise``，resolve 出与 ``host.http.request`` 相同的 ``{ status, body, headers? }`` 对象。

Attention
^^^^^^^^^

最多 **4** 个并发请求；第 5 个不会排队，直接 **reject**，请务必处理 rejection。传输失败同样走
rejection —— Promise 若悬而未决会让 ``await`` 永久卡住。**没有同步降级**：恢复 ``await`` 需要
worker 排空 QuickJS 待处理任务，所以在不支持本接口的 Host 上，
``await Promise.resolve(host.http.request(opts))`` 同样不会继续，而且仍然阻塞 worker。
使用本接口的模块须写明 Host 要求（见 ``examples/plugins/`` 与插件 README 的 “Host 要求”）。

方法 — host.net
---------------

IPv4 UDP。二进制载荷使用 **base64**。实现：``components/minihub_js/minihub_js_ext.c``。

host.net.udp.open(opts?)
~~~~~~~~~~~~~~~~~~~~~~~~

``opts.broadcast`` （可选 bool）开启广播。返回小整数 socket 句柄。

host.net.udp.sendto(fd, { ip, port, data_b64 })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

host.net.udp.recvfrom(fd, { timeout_ms, max_len? })
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

返回 ``{ ip, port, data_b64 }``，超时为 ``null``。

host.net.udp.close(fd)
~~~~~~~~~~~~~~~~~~~~~~

方法 — host.crypto
------------------

摘要 / HMAC / AES。摘要返回 **小写 hex**；HMAC 与 AES 返回 **base64**。
输入可为字符串或 ``{ data }`` / ``{ data_b64 }``。

* ``host.crypto.md5(input)``
* ``host.crypto.sha1(input)``
* ``host.crypto.sha256(input)``
* ``host.crypto.hmacSha256({ key_b64, data_b64 })`` → base64
* ``host.crypto.aes128CbcEncrypt({ key_b64, iv_b64, data_b64 })`` → base64（PKCS7）
* ``host.crypto.aes128CbcDecrypt({ key_b64, iv_b64, data_b64 })`` → base64

示例：``examples/plugins/color_bulb_js/``。

方法 — host.config
------------------

host.config.get()
~~~~~~~~~~~~~~~~~

只读返回当前 ``adapters.json`` 根对象。

Returns
^^^^^^^

JavaScript 对象，或 ``null``。

host.config.save(adaptersRoot)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

写回完整 ``adapters.json`` （``minihub_config_save_adapters``）。

Parameters
^^^^^^^^^^

``adaptersRoot`` （object，必填）
    要写入的整份 adapters 文档。

Returns
^^^^^^^

``undefined``；失败时 **抛异常**。

方法 — host.module
------------------

host.module.rpc(moduleId, method, params?)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

调用其它已加载模块的 RPC（``minihub_module_invoke_rpc``），例如一个商店
模块调用另一个模块的共享能力。

Parameters
^^^^^^^^^^

``moduleId`` （string，必填）
    目标模块 id。

``method`` （string，必填）
    RPC 方法名。

``params`` （object，可选）
    JSON 参数。

Returns
^^^^^^^

解析后的 JSON 对象（或 ``null``）。失败时 **抛异常**。

host.module.registerRpc(method, handler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

为本模块 id 注册 RPC（仅可在 ``setup`` 内调用）。Web UI 与
``POST /api/modules/<id>/rpc`` 会调用 ``handler(method, params)``。

.. only:: not hide_l3

   在 **hybrid** 模块上，JS 处理器为 RPC 主路径；未在 JS 注册的方法会回落到
   原生 ``minihub_module_rpc``。

Parameters
^^^^^^^^^^

``method`` （string，必填）
    方法名（如 ``login``、``sync``）。

``handler`` （function，必填）
    返回可 JSON 序列化的对象。返回 ``false`` 或抛异常表示失败。

Returns
^^^^^^^

``undefined``。

见 :doc:`../plugins/develop_l2`。

开发 JS（JavaScript）插件
=========================

:link_to_translation:`en:[English]`

JS 模块在 Hub 上运行 **QuickJS** 脚本。Host 在适配器启动后及软重载
（``POST /api/hub/reload`` ）时加载 ``main.js``。实现见
``components/minihub_js/minihub_js.c``。

运行环境
--------

引擎是 QuickJS-NG，也就是 Bellard 那份 QuickJS 的社区分支；本固件把
``espressif/quickjs-ng`` 组件固定在 0.14.0。语法与能力边界以 ``quickjs-ng`` 组件
文档及实际链入版本为准；``class``、``async`` / ``await``、 ``Proxy``、可选链均可
使用。MiniHub 另提供四个全局定时器和一个 ``host`` 对象，无其它运行插件，因此
JS 模块既不是浏览器页面，也不是 Node 进程。

与浏览器 / Node 环境的主要差异：

* 只有一个文件，没有模块系统。Host 把每个 ``main.js`` 放进各自的函数里执行
  （见 ``minihub_js.c`` ），顶层的 ``function`` 和 ``var`` 只属于这个模块，不会盖掉
  另一个模块里的同名函数。``import`` 和 ``require`` 仍然什么都解析不到；也没有
  读文件的 Host 接口 —— 每个模块只有一份 ``main.js``。
* 没有 ``console``，也没有 ``print``。打日志用 ``host.log.info`` /
  ``host.log.warn`` / ``host.log.error``。
* 没有 ``fetch``、没有 ``XMLHttpRequest``、没有文件系统。出网走
  ``host.http.request`` / ``host.http.requestAsync``，要存东西走
  ``host.config``。见 :doc:`../api/js_host`。
* ``setTimeout`` / ``setInterval`` / ``clearTimeout`` / ``clearInterval`` 既装在
  全局，也在 ``host.timer`` 下重复一份。所有回调都跑在同一个 JS 工作线程上，
  一个回调卡住就会拖住其它 JS 模块。
* 工作线程栈是 ``CONFIG_MINIHUB_JS_WORKER_STACK_SIZE`` （默认 32768 字节，开在
  PSRAM 上），QuickJS 的递归上限按 ``CONFIG_MINIHUB_JS_WORKER_STACK_BUDGET``
  （默认 24576）设，让递归先撞到引擎限制而不是真实栈。
* JS 需要 ``CONFIG_MINIHUB_JS_PLUGINS``，打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES``
  后它默认就是开的。

适用场景
--------

* 轻量自动化、HTTP 轮询、REST 与点位之间的胶水逻辑
* 通过 ``/modules/`` 提供 UI 片段，无需原生代码

.. only:: not hide_l3

   * 在投入 ELF 之前快速验证想法

要求：产品 sdkconfig 打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` （``app.yml`` 中
``dynamic_modules`` 特性）。

目录结构
--------

.. code-block:: text

   examples/plugins/ID/
     module.json
     main.js              # 必需 —— 里面定义 setup(host)；没定义时模块仍算加载成功，
                          # 只留一条 no setup(host) function 告警，也不会登记任何点
     meta.json            # 可选 — ui + capabilities
     web/                 # 可选静态资源

``module.json`` 示例
--------------------

.. code-block:: JSON

   {
     "id": "demo_js",
     "name": "Demo JS 模块",
     "version": "0.1.0",
     "enabled": true,
     "type": "module",
     "runtime": "js",
     "adapter": "demo_js",
     "targets": ["any"],
     "requires": []
   }

``requires`` 列出这个模块需要的能力，取自 ``Zigbee`` 、 ``BLE_mesh`` 、 ``Thread_Matter`` 、
``Matter`` 、 ``MQTT_Broker`` 、 ``Modbus`` 。 ``[]`` 表示不需要。目录请求带 ``caps=`` ，
内容是这台 Hub 当前已启用的能力。列出的任一项不具备时，Hub 商店不把该模块放进可安装列表。
已经安装的模块下次上电不启动，商店里标为当前固件不支持。 ``targets`` 用 ``any`` 表示任意芯片；
写 ``ALL`` 也会按 ``any`` 保存。

JS manifest 不带 ``abi_version`` —— 那属于原生包。``ui`` 与 ``capabilities``
随包里有 ``meta.json`` 时从它读，没有则回落到 ``module.json`` 里的同名字段。

**``runtime: "js"`` 是必填。** 省略它并不会被当成 JS：加载时运行时只在
``runtime`` 恰为字符串 ``JS`` （或 ``hybrid``）时才启动 QuickJS，既没有
``runtime`` 又没有 ``binary`` 的 manifest 会走原生那条路，报
“no binary or builtin” 而失败。别处可能看到过的「有 ``main.js``、无
``CMakeLists.txt`` 就算 JS」那个启发式，是 **打包器与目录生成器** 给源码树分级用的，
Hub 不用它。``adapter`` 同样必填 —— 适配器名为空的 manifest 通不过校验，JS 也不例外。

``main.js`` 约定
----------------

Host 每次加载调用 ``setup(host)``。``host`` 对象提供：

* ``host.log.info/warn/error(msg)``
* ``host.point.register({ point_id, type, adapter, value, online, props|params })`` —
  一台设备一条点（``协议.设备``）。``type`` 须是类型目录里的 ``device_id``
  （如 ``light`` / ``switch``），概览页才有标准控件；见 :doc:`../type_catalog`。
  传入 ``params`` （简写）或完整 ``props``，使 ``props.params`` 到达 UI。
* ``host.point.get(point_id)`` · ``host.point.set(point_id, value, props?)`` ·
  ``host.point.unregister(point_id)``
* ``host.event.subscribe(event_type, handler)`` — ``point.updated`` /
  ``point.registered`` （在同一 Host ``point_id`` 上补充字段）
* ``host.command.on(adapter, handler)`` — 在 JS 中接收 UI/REST 命令
* ``host.command.invoke({ point_id, cmd, params? })``
* ``host.adapter.poll(fn)`` — 在约 5 秒的适配器轮询节拍下运行 ``fn``，整个运行时
  最多 8 个；``host.adapter.clearPoll()`` 会清掉全部回调
* ``host.http.request({ url, method, headers, body, timeout_ms, response_b64 })``
  —— 同步 HTTP **跑在 JS worker 上**，这期间整个 JS 运行时都被它占住。设备里没有
  URL 白名单这回事：能不能连通、以及 ``https`` 的证书信任根，就是全部限制。
  ``response_b64`` 让响应体按 base64 返回，而不是当 UTF-8 字符串
* ``host.http.requestAsync(opts)`` — ``opts`` 同上，返回 **Promise**，socket 收发不占用
  JS worker（最多 4 个并发）。多请求链路（云端批量刷新）用它；旧 Host 没有同步降级
* ``host.config.get()``

插件要丰富 Zigbee（或其他 Host）设备时必须用 **同一** ``point_id``。
扩展写在顶层 ``props``：``manufacturer`` / ``category`` / ``ota`` /
``owner`` 是身份与分组元数据，不是类型目录参数。适配器遥测只合并
``props.params``，因此这些扩展会保留。自建点应在 ``register`` 时带上厂商
与品类（演示 ``color_bulb_js``）。完整 Host API：:doc:`../api/js_host`。

概览页演示（三盏灯）：``examples/plugins/color_bulb_js/``。

最小 ``main.js``：

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

示例里的 ``sensor`` 是省略 ``type`` 时加载器填进去的默认值（见 ``minihub_js.c`` ），
而它 **不是** 类型目录里的 ``device_id`` ：点仍会登记、仍可进入规则和 REST ，
但控制台画不出控件卡片，走的是「类型未登记」的退化渲染 —— 参数名在参数表里有条目的照条目画，
其余画成一行只读文本（:doc:`../type_catalog` ）。要卡片就写真实的 ``device_id``
（ ``light`` / ``switch`` / ``sensor_contact`` 等）。

可选 ``meta.json`` 描述 Web UI 中的模块表单（完整示例见
``examples/plugins/color_bulb_js/`` ）。

接入 ``adapters.json``
----------------------

增加适配器块（``devices`` 可为空）：

.. code-block:: JSON

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

然后在 Web UI（ ``http://HUB/`` ）中打开 ``商店``，将自定义商店 URL 设为
``http://YOUR_PC:8080``，刷新目录并安装 ``demo_js``。Hub 会自动重载模块。

验证：

* ``GET /api/modules`` — ``demo_js`` 已列出且 ``loaded: true``
* ``GET /api/points`` — 可见 ``demo_js.hello``
* 串口：``JS module 'demo_js' setup ok``

JS 可选模块 RPC；更推荐 ``host.command`` 或 REST。API 详解 :doc:`../api/js_host`。

.. only:: not hide_l3

   ELF RPC 见 :doc:`develop_elf`。

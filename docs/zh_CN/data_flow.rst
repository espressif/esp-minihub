端到端数据流
============

:link_to_translation:`en:[English]`

.. contents::
   :local:
   :depth: 2

一次控制动作构成完整回路：用户在控件上触发命令，经适配器下发至设备；设备上报新
状态后写入点位，并以 ``point.updated`` 进入事件总线；总线同步更新界面并触发规则，
规则动作可开启下一轮。命令未送达设备时，仍会发出 ``command.invoked``，但不会伴随
``point.updated``，回路在调用方终止。参与的四个子系统是 ``minihub_device_widget``
（类型目录）、商店模块、``minihub_rules`` 与 Web API。每一环只把值交出去，并不知道
下一环是谁。

.. image:: ../_static/data_flow/data_flow.svg
   :align: center
   :alt: MiniHub 端到端数据流：控制 → 设备 → 点位更新 → 事件总线 → 屏幕/规则 → 商店模块

`交互版 <_static/data_flow/index.html>`_ — 按回路顺序展示各子系统；每张卡片对应一项约束（悬停可暂停）。

静态结构（哪一层允许依赖哪一层）见 :doc:`architecture`。字段定义见
:doc:`data_model` 与 :doc:`type_catalog`。

依赖方向
--------

依赖单向向下。射频层的改动因此不会经由编译期依赖传到 UI。 ``minihub_web`` 依赖
``minihub_device_widget`` 与 ``minihub_core``；设备控件组件是独立的，编译期并不
依赖 Core。适配器把点注册进 Core，Core 以下不会回调 Web。 :doc:`architecture`
里的分层图画的是同一个栈，从入口一直到芯片。

设备侧 MCP 服务器（ ``CONFIG_MINIHUB_MCP_SERVER`` ）是另一条入口：它按需读取
同一套点注册表、调用同一个命令路由器，因此它的工具看到的点与浏览器完全一致。
它并 **不** 订阅事件总线，与 ``tools/`` 下给插件作者用的 IDE MCP 也无关。

一次控制的完整回路
------------------

下面按这个回路逐段展开。

1. 控制
^^^^^^^

   点击控件调用 ``POST /api/devices/control`` （处理函数
   ``handle_api_devices_control``，位于 ``components/minihub_web/minihub_web.c``），
   JSON 为
   ``{ "device": 类型, "parameter": 名, "value": …, "point_id": id, "seq": n }``。
   ``device`` 是 **数据模型类型** （``light``、``switch`` 等）。只有当点存在、
   类型匹配、**且** 该点确实带有这个参数时，Host 才会用 ``point_id``；否则回退到
   ``(类型, 参数)`` 的首次匹配，仍然找不到则返回 **404** 。请求体读进一个固定的
   1 KiB 缓冲，能接受的最大请求体是 1023 字节；``value`` 经 JSON 序列化，字符串带
   引号，放不进 256 字节取值缓冲的一律拒绝；布尔值转为 ``toggle`` 命令，其余转为
   ``set``。``seq`` 是客户端时间戳 （取 ``Date.now()`` 的低 32 位），只随响应
   ``payload`` 回传，从不上总线。响应为
   ``{ "result", "path", "parameter", "payload" }``；失败码为 **401** （先校验会话，
   在读请求体之前）、**400** （请求体缺失、非法或过大，或取值过长无法序列化）、
   **404** （点不存在）、**500** （适配器调用失败或内存不足）。这条路
   没有"忙"状态码。

   所有命令来源都汇到同一个路由器（``minihub_command_invoke``）：上面的控件
   POST、带明确 ``point_id`` 的 ``POST /api/command`` （可选
   ``params.parameter``）、规则的 ``command`` / ``command_by_type`` 动作，以及
   JS 的 ``host.command.invoke`` / ``host.command.on``。路由器解析到点后，
   由该点的适配器把命令经射频发给设备。

2. 设备执行与上报
^^^^^^^^^^^^^^^^^

   适配器经各自的射频（Zigbee、Matter、BLE Mesh 等）把命令送达设备；设备执行后
   把新状态报回适配器。协议细节见 :doc:`zigbee`、 :doc:`matter`、
   :doc:`ble_mesh`。送达失败时适配器返回错误，路由器记一条告警；此时
   ``command.invoked`` 早已发出（见第 4 步），总线上看得见这次尝试，但不会有
   ``point.updated`` 跟着发出——回路就此不闭合，调用方只能从 HTTP 状态得知失败。

3. 点位更新
^^^^^^^^^^^

   适配器把新值写回该注册点（``minihub_point_t``）上的结构化 JSON，典型为
   ``props.params.<name>.value``；实时参数、访问权限、可见性、实例上下界都放在
   同一个点上。点 id 如 ``zigbee.zb_7d4b``、``thread.matter_n2_ep1``、
   ``sim.js_kitchen``；点还带一个 ``type`` 字符串（如 ``light``）表示数据模型
   类型。详见 :doc:`data_model`。

4. 事件总线
^^^^^^^^^^^

   值变了、或 ``props`` 变了，才会发出 ``point.updated``；值与 ``props`` 都没变，
   再写一次什么都不会发出。一连串写入里第一次立即发布，落在
   去抖窗口（``CONFIG_MINIHUB_POINT_DEBOUNCE_MS``，默认
   1 秒）内的后续写入只更新点位并留下一次待发布，窗口过完由周期扫描补发一次——
   拖动滑块因此发布的是起始值和最终值，而不是每一个中间值。事件携带点的注册表下标
   ``point_index``（不是点 id）以及 ``old_value`` / ``new_value``。

   总线是 ``minihub_core`` 里的真实抽象：订阅表（32 槽）、由单个派发任务消费的
   FreeRTOS 队列（深 32），以及每条订阅都带的属主标签 （通配订阅也不例外）——模块
   卸载因此不会留下悬空订阅者。投递是尽力而为：队列满即丢事件，消费者必须容忍缺口。
   同一条总线上还有 ``point.registered`` / ``point.unregistered``、
   ``command.invoked``（在适配器执行 **之前** 发布，成败都会发；点和它的适配器处理
   函数根本无法解析时不发）、``hub.reloaded``、``device_widgets.updated``
   与 ``store.module.installed``。

5. 消费者：屏幕与规则
^^^^^^^^^^^^^^^^^^^^^

   ``point.updated`` 有多个消费者，且没有哪一个特殊：

   * **屏幕。** ``boot.js`` 持有 WebSocket ``/ws/events``
     （``components/minihub_web/minihub_web_ws.c``）。该 socket 订阅**全部**
     事件，由浏览器按 ``event_type`` 过滤。``point.updated``（或
     ``hub.reloaded``）会调度一次去抖后的 ``GET /api/points`` 重新拉取，随后
     ``overview.js`` 重画网格——回路在此闭合，用户看到新值。
     ``device_widgets.js`` 提供目录求交与详情视图里的逐参数控件。socket 表只有 8
     个槽位：第 9 个连接不会被拒绝，只是再也收不到广播，连接仍在但事件已停；
     断线按退避重连（2 秒起、×1.5、上限 15 秒）并重新拉取，断开期间每 3 秒轮询兜底。
   * **规则引擎** （``minihub_rules``）逐事件评估 ``when`` / ``if`` / ``then``
     ——上面的去抖与同值抑制因此同样作用于规则。命中后执行 ``command``、
     ``command_by_type``、``delay`` 或 ``notify`` （见 :doc:`rules`）。
     ``command`` 类动作回到第 1 步，开启下一轮回路；场景是独立的
     ``POST /api/scenes/invoke``，不是规则 ``then`` 类型。
   * **商店模块** 调用 ``host.event.subscribe(type, fn)`` 订阅（见第 6 步）。

   数值历史记录在点写入的内联路径里，而非总线上：只记变化值，PSRAM 中 160 条
   环形缓冲，``GET /api/points/history`` 最多返回 64 条，重启即清空。

6. 商店模块
^^^^^^^^^^^^^

   第三方行为以 **商店模块** 形式交付，位于 ``/store/modules/ID/``：

   * **JSON** ``minihub_pack`` —— 注入已有适配器（不引入新射频协议栈）。
   * **JS** ``minihub_module`` —— JavaScript 插件（``main.js``，经 ``minihub_js``
     的 QuickJS）；可附带 ``web/``，由 ``/modules/ID/web/…`` 提供。

.. only:: not hide_l3

   * **ELF** ``minihub_module`` —— 运行时加载的原生 ``.so``。

模块用 ``host.event.subscribe(type, fn)`` 订阅（内部是一次通配订阅，在 JS 侧
过滤）、调用 Host API、注册点——模块注册的点与其他点一样发出
``point.updated``，因此第 4 步的事件同样会到达模块。可安装模块的目录即
**商店** （``minihub_store``）。演示放在 ``examples/plugins/`` （经
``MINIHUB_PLUGIN_DIRS``）；公开 Host 的 ``store_preinstall`` 默认留空，除非
有意内置出厂模块。模块绝不在 Core 中写入厂商令牌。

这个回路保证什么
----------------

回路按设计是尽力而为，保证很窄：

* **真正的空写入不可见。** 同一个值配上同样的 ``props`` 再写一次不会发出事件，因此
  既不重画也不评估规则；只改 ``props`` 是会发的。
* **突发被合并。** 去抖窗口（默认 1 秒）意味着屏幕与规则看到的是稳定值，而非
  每一个中间值。
* **事件可能丢失。** 总线队列有限，高负载下消费者可能错过事件。屏幕靠重新拉取
  自愈；规则只能看到送达的事件。
* **后写者胜。** 两个客户端（或客户端与规则）命令同一个点会互相竞争，彼此之间
  没有任何排序。点写入由注册表互斥锁保证原子，但命令并不串行：路由器在调用处理
  函数之前就放开适配器生命周期锁，好让一条走 LAN/云的命令卡不住
  ``GET /api/adapters``；这把锁管的是处理函数查表与 reload 引用，不是那次设备
  调用本身。
* **历史易失。** ``GET /api/points/history`` 只读内存，重启即清空。

开机时的一次性准备
------------------

上面的回路依赖两样开机时就准备好的东西：注册点与可渲染的卡片。

1. 发现与注册
^^^^^^^^^^^^^

   适配器（如 Zigbee、Matter、BLE Mesh）发现设备后，为每个设备注册
   一个 ``minihub_point_t`` ：Matter 按 endpoint，Modbus 按 register-map 条目，
   一个设备因此可以合法地占着多个点。注册按 id 幂等——已存在的点被更新，不会重复登记。Thread
   适配器只做 Border Router，**不** 登记设备点，见 :doc:`thread`。没有这一步，
   第 1 步的 ``point_id`` 无从解析。

2. 类型目录与渲染
^^^^^^^^^^^^^^^^^

   一个 ``type`` 如何渲染是 **声明式** 描述的，而非写在 C 里。四个模型 CSV 里
   只有两个进入运行时注册表：``devices.csv``（类型 → 参数、主控件、图片）与
   ``parameters.csv``（逐参数模式：控件、选项、访问权限、单位）。
   ``services.csv`` 只是参考资料（RainMaker 系统服务），从不进入注册表；
   ``ui_elements.csv`` 是闭合控件集，``parameters.csv`` 的 ``ui_element`` 列
   对照它校验。构建期由
   ``components/minihub_device_widget/tools/gen_device_widget.py`` 把
   ``devices.csv`` 与 ``parameters.csv`` 折叠成一份扁平的注册表文本并编译进固件
   （``MINIHUB_DW_EMBEDDED_CSV``）。扁平化发生在构建期，不在固件里：开机 seed 到
   ``/data/config/devices.csv`` 的就是这段文本的原样，只有在运行时上传的 model CSV
   才走运行时扁平化。启动时 ``minihub_device_widget_init``
   在文件缺失或损坏时从内嵌副本 seed，再加载它。

   该目录是 **协议无关** 的：不含 transport 字段。适配器前缀（``zigbee.`` /
   Matter 的 ``thread.matter_n…`` 或 ``wifi.matter_n…`` 等）来自点的注册，
   因此同样的 ``light`` /
   ``switch`` 控件不论设备以何种方式接入都能渲染。内置类型表、控件与开发
   用法见 :doc:`type_catalog`。

   概览页发起 ``GET /api/device_widgets`` （处理函数
   ``handle_api_device_widgets`` 位于 ``components/minihub_web/minihub_web.c``）
   以及 ``GET /api/points``；``device_widgets.js`` 把类型目录与每个实时点的
   ``props.params`` 求交，由 ``overview.js`` 绘制网格。控件类型是
   ``components/minihub_device_widget/model/ui_elements.csv`` 里的闭合集合，
   各自的画法和取值见 :doc:`type_catalog`。每张卡片对应一个注册点，绝不为每个参数各画一张
   卡片；逐参数控件在详情视图里。设备头像由 ``GET /img/devices/…`` 提供。没有
   这一步，第 1 步的卡片不存在。

类型目录的运行时约定
--------------------

目录可在 **不重新编译固件** 的情况下通过
``PUT /api/dev/catalog/{devices.csv|parameters.csv}`` 编辑（见
:doc:`api/host_rest`）。reload 路径被设计为可容错：

* **原子 reload。** 新表先完整构建，再交换内存中的注册表。若传入文件无法解析，
  则保留旧表并使请求失败——UI 永远不会读到半释放的注册表。
* **坏行跳过而非致命。** 单行格式错误会被记录并丢弃，其余好行仍加载（旧行为
  是任一行出错就整表拒绝并覆盖自定义内容）。
* **类型未登记会留下一行日志。** 当某个点的 ``type`` 为空或不在目录中时，设备详情
  仍会画它的参数行，浏览器控制台打印 ``type not in catalog``，从而把数据问题与
  渲染问题区分开。
* **自愈。** 若启动时 ``/data/config/devices.csv`` 缺失或损坏，固件会用内嵌
  副本重新 seed 并 reload。
* **两个文件，一个模型。** 只 ``PUT`` 其中一个模型 CSV 会返回 OK 并附提示，
  但要等另一个也到位才真正重建注册表；``DELETE /api/dev/catalog/*`` 删掉运行时
  副本，回退到内嵌副本。

API 速查
--------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - 端点
     - 用途
   * - ``GET /api/points``
     - 列出已注册点（设备）及其 ``props.params``
   * - ``GET /api/points/history``
     - 单点数值历史
   * - ``POST /api/points/remove``
     - 遗忘一个点
   * - ``POST /api/points/rename``
     - 改一个点的显示名
   * - ``GET /api/device_widgets``
     - 类型目录（文案 / 控件）；可选第一条匹配的 ``point_id``
   * - ``POST /api/devices/control``
     - 控件控制：``device`` 是类型；可选 ``point_id`` 指定实例
   * - ``POST /api/command``
     - 按 ``point_id`` 发命令（可选 ``params.parameter``）
   * - ``GET /api/adapters``
     - 适配器概览（启用 / 模块）；不是完整 ``adapters.json``
   * - ``GET|PUT /api/config/adapters``
     - 读取 / 替换 adapters 文档
   * - ``GET|PUT /api/dev/catalog/{devices.csv|parameters.csv}``
     - 运行时读取 / 替换类型目录
   * - ``DELETE /api/dev/catalog/{devices.csv|parameters.csv}``
     - 删掉运行时副本，回退到内嵌副本
   * - ``GET /api/rules``、``POST /api/rules/import``、``POST /api/rules/ai/chat``
     - 规则管理与 AI 助手
   * - ``GET /api/modules``、``POST /api/modules``、``POST /api/modules/*``
     - 商店模块列表 / 安装 / RPC
   * - ``GET /api/modules/*/preview``
     - 模块 UI 预览
   * - ``GET /api/packs``、``POST /api/packs``
     - JSON pack 列表 / 管理
   * - ``GET /api/scenes``、``POST /api/scenes/import``、``POST /api/scenes/invoke``
     - 场景管理
   * - ``GET /api/events``、WebSocket ``/ws/events``
     - 近期事件环（REST 轮询）加实时 socket
   * - ``GET /img/`` 加子目录与文件名
     - 设备 / 参数图标（已做路径穿越防护）
   * - ``POST /api/hub/reload``
     - Hub 运行时软重载（会重跑目录 reload）

下一步
------

* :doc:`type_catalog` —— 设备类型、控件与图标。
* :doc:`data_model` —— 点位模型与 adapters / 场景。
* :doc:`rules` —— ``when`` / ``if`` / ``then`` 与比较 ``op``。
* :doc:`web_console` —— 替换或换肤 Web 控制台。
* :doc:`plugins/index` —— 编写 JSON/JS 商店模块。
* :doc:`api/index` —— 完整 REST / JS / MCP / C ABI 参考。

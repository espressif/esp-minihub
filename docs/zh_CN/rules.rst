规则引擎
========

:link_to_translation:`en:[English]`

Host 自动化写在 ``rules.json`` （``minihub_rules``）。一条规则包含 ``when``
（触发）、可选 ``if`` （附加条件）和 ``then`` （动作）。场景
（``scenes.json``）是 **独立** 文档，由 REST 调用； **不是** ``then`` 类型。

实现：``components/minihub_rules/minihub_rules.c``。持久化与 REST：
``GET /api/rules``、``POST /api/rules/import`` （:doc:`api/host_rest`）。

文档形状
--------

.. code-block:: json

   {
     "rules": [
       {
         "id": "door_light",
         "name": "开门开灯",
         "enabled": true,
         "when": {
           "type": "point",
           "point_id": "sim.color_bulb",
           "parameter": "brightness",
           "op": "gt",
           "value": "50",
           "delay_sec": 0
         },
         "if": [ { "point_id": "sim.meter_plug", "parameter": "power", "op": "eq", "value": "on" } ],
         "then": [
           {
             "actions": [
               { "type": "command", "point_id": "sim.color_bulb", "cmd": "turn_on" },
               { "type": "notify", "message": "门已打开" }
             ],
             "delay_after": 5
           }
         ]
       }
     ]
   }

``when``
--------

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - ``type``
     - 行为
   * - ``point``
     - Hub 事件 ``point.updated`` 时触发。推荐 ``parameter`` + ``op`` +
       ``value``，比较 ``props.params.名称.value``。旧字段 ``equals`` 仍匹配
       点位摘要 ``value``。
   * - ``time``
     - 每天 ``at`` （``HH:MM``）触发一次；需要 SNTP。

``point`` 与 ``time`` 均可选 ``delay_sec``：触发后等待该秒数再执行第一步。
值为 0 表示“立即执行”，是默认值。

``if`` 为附加条件数组。每项形状与 point 的 ``when`` 相同（须全部满足）。

比较 ``op``
-----------

在 ``normalize_op`` 中规范化。未知 token 视为 ``eq``。

.. list-table::
   :header-rows: 1
   :widths: 16 28 56

   * - 规范名
     - 别名
     - 含义
   * - ``eq``
     - ``=`` ``==``
     - 等于（默认）
   * - ``ne``
     - ``!=`` ``<>``
     - 不等于
   * - ``lt``
     - ``<``
     - 小于
   * - ``le``
     - ``<=`` ``≦``
     - 小于等于
   * - ``gt``
     - ``>``
     - 大于
   * - ``ge``
     - ``>=`` ``≧``
     - 大于等于

``then`` — 步骤
--------------------

``then`` 是一个 **有序的步骤列表**。每个步骤是一个对象，包含：

* ``actions`` —— 一起执行的动作数组（每步最多 4 个）。
* ``delay_after`` —— 本步完成后、进入下一步前等待的秒数。最后一步忽略；
  0 表示无延迟。

步骤串行执行：第 0 步在 ``when.delay_sec`` 之后运行，后续每步在前一步的
``delay_after`` 之后开始。这取代了旧的独立 ``delay`` 动作——延迟现在属于“前
一个步骤”，而不再是单独的动作。旧的扁平 ``then``（单个动作对象或数组，可含
内联 ``delay`` 动作）固件仍会解析为步骤。

.. code-block:: text

   when(触发) --[when.delay_sec]--> 步骤0动作 --[delay_after]--> 步骤1动作 --> ...

Web 编辑器里，等待时间由该步骤的「动作类型」下拉选择 ``延迟`` 得到（没有独立
的延迟输入框）：延迟总是落在该步骤末尾，一个步骤最多一个。正因为延迟才是分隔
两个时刻的东西，所以**只有**当最后一个步骤以延迟结尾时，才会出现 ``+ 添加步骤``
。

动作（``type``）
~~~~~~~~~~~~~~~~~~~

**只有** 下列三种 ``type`` 会执行（没有 ``scene`` 动作，也不直接调用
``minihub_point_set``）。命令走 ``minihub_command_invoke`` （与
``POST /api/command`` 同一路径）。跑场景请用 ``POST /api/scenes/invoke``，不要
写在规则 ``then`` 里。

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - ``type``
     - 字段
   * - ``command``
     - ``point_id`` + ``cmd`` （与 REST 相同；开关用 ``turn_on`` /
       ``turn_off`` / ``toggle``）。取值类命令可用可选 ``params``
       （``brightness``、``color_temp``、``color``）。
   * - ``command_by_type``
     - ``cmd`` 与可选 ``types`` CSV（默认 ``light,switch``）。匹配点还须通过
       Host 的灯/插座过滤（跳过 ``mock``、PIR/人体感应类名称）。典型用途：
       「关掉所有灯」。固件仍会执行，但编辑器不再提供该选项；已有规则仍可编辑
       并继续运行。
   * - ``notify``
     - ``message``。若产品链接了 IM 钩子则发送，否则打警告日志。

未知 ``then.type`` 记日志并跳过。

执行为 **非阻塞**：延迟由一次性 ``esp_timer`` 回调驱动，回调只把执行任务
投递到专用任务队列——命令绝不在定时器回调内执行。每次重载都会递增一个单调递增的
代际计数器，因此重载前已挂起的延迟任务会被丢弃（重载后无悬空执行 / 野指针）。

导入与 MCP
----------

* REST 导入：``POST /api/rules/import``，body 为文档本身或
  ``{"url":"http://…"}``。
* MCP：``rules.list`` / ``rules.put``。

相关
----

* 点位模型：:doc:`data_model`
* 场景：:doc:`data_model` （``scenes.json``）
* REST：:doc:`api/host_rest`
* 端到端流程：:doc:`architecture_overview`
* ``/ws/events`` 事件类型：:doc:`api/host_rest`

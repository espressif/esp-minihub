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
           "value": "50"
         },
         "if": [ { "point_id": "sim.meter_plug", "parameter": "power", "op": "eq", "value": "on" } ],
         "then": [
           { "type": "command", "point_id": "sim.color_bulb", "cmd": "turn_on" },
           { "type": "notify", "message": "门已打开" }
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

``then``
--------

对象或数组。 **只有** 下列四种 ``type`` 会执行。没有 ``scene`` 动作，也不直接
调用 ``minihub_point_set``。命令走 ``minihub_command_invoke`` （与
``POST /api/command`` 同一路径）。跑场景请用 ``POST /api/scenes/invoke``，不要
写在规则 ``then`` 里。

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - ``type``
     - 字段
   * - ``command``
     - ``point_id`` + ``cmd`` （与 REST 相同；开关用 ``turn_on`` /
       ``turn_off`` / ``toggle``）。
   * - ``command_by_type``
     - ``cmd`` 与可选 ``types`` CSV（默认 ``light,switch``）。匹配点还须通过
       Host 的灯/插座过滤（跳过 ``mock``、PIR/人体感应类名称）。典型用途：
       「关掉所有灯」。
   * - ``delay``
     - ``seconds``。用规则任务上的 ``vTaskDelay`` 实现，会 **阻塞** 后续
       ``then`` 以及该任务上的其它规则。慎用；顺序延迟更适合场景的
       ``delay_ms``。
   * - ``notify``
     - ``message``。若产品链接了 IM 钩子则发送，否则打警告日志。

未知 ``then.type`` 记日志并跳过。

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

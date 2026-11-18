规则引擎
========

:link_to_translation:`en:[English]`

自动化在设备上求值，不在云端，所以断网时 Hub 仍能继续工作。规则写在
``rules.json``，由 ``minihub_rules`` 求值。一条规则分三段： ``when`` 负责触发，
可选 ``if`` 追加条件， ``then`` 是触发后要做的事。

.. image:: ../_static/rules/rules_flow.svg
   :align: center
   :alt: MiniHub 规则求值：点位更新 → 触发 → 附加条件 → 步骤 → 动作

`交互版 <_static/rules/index.html>`_ —— 按求值顺序展示各阶段；每一站对应一项约束（悬停可暂停）。

实现：``components/minihub_rules/minihub_rules.c``。持久化与 REST：
``GET /api/rules``、``POST /api/rules/import`` （:doc:`api/host_rest`）。

文档形状
--------

.. code-block:: JSON

   {
     "rules": [
       {
         "id": "door_light",
         "name": "开门开灯",
         "enabled": true,
         "when": {
           "type": "point",
           "point_id": "sim.js_kitchen",
           "parameter": "brightness",
           "op": "gt",
           "value": "50",
           "delay_sec": 0
         },
         "if": [ { "point_id": "sim.js_plug", "parameter": "power", "op": "eq", "value": "on" } ],
         "then": [
           {
             "actions": [
               { "type": "command", "point_id": "sim.js_kitchen", "cmd": "turn_on" }
             ],
             "delay_after": 5
           },
           {
             "actions": [
               { "type": "notify", "message": "门已打开" }
             ]
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
       ``value``，比较 ``props.params.名称.value``；不写 ``parameter`` 时比较
       点位摘要 ``value``。两种写法都是 **边沿触发**：值 *变化进入* 命中状态才
       触发，所以设备重新同步把同一个值再发一遍不会重复触发。
   * - ``time``
     - 每天时钟读到 ``at`` （``HH:MM``，按 UTC 保存）时触发一次。时间规则由一个
       每秒轮询的任务检查，匹配精度到分钟，所以实际触发在整分后约 1 秒内。
   * - ``once``
     - 只触发一次，``at`` 写成 ``YYYY-MM-DD HH:MM`` （UTC，精确到分钟）。触发后
       该规则被标记为已完成，直到规则重新加载。

时间规则只要求系统日历年份 ≥ 2020，并不要求 SNTP 同步完成，因此纯局域网、手工
校时的 Hub 也可跑定时。时间来源与未同步时的行为见 :doc:`time` 。

在出现“系统时区”设置之前创建的时间规则，其 ``at`` 是按编辑时*浏览器*时区换算
成 UTC 保存的。系统页选定时区后，控制台改为按系统（设备）时区读写 ``at``，因此
这类旧规则的实际触发时刻会相对原定时偏移（浏览器时区 − 设备时区）的小时数：
浏览器 UTC+8、设备 UTC 时偏 8 小时。重新打开并保存一次该规则，即可把它重新
锚定到设备时区。

``point`` 与 ``time`` 均可选 ``delay_sec``：触发后等待该秒数再执行第一步。
值为 0 表示“立即执行”，是默认值。控制台没有这一项的输入框 —— 要用它就得写进
``rules.json`` 再重新导入；编辑器里唯一能设的延时是步骤内的那段等待。

``if`` 为附加条件数组，最多 **4** 条，须全部满足。每项形状与 point 的
``when`` 相同（``point_id``、``parameter``、``op``、``value``）。``if`` 只在规则
被触发时比对当前点位状态，它自己不会触发任何事。

比较 ``op``
-----------

在 ``normalize_op`` 中规范化。未知或缺失的 token 视为 ``eq``。

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

点位侧的值都是字符串，所以 ``eq`` / ``ne`` 先做宽松比较：``on`` / ``true`` /
``1`` 算一类，``off`` / ``false`` / ``0`` 算另一类（大小写不敏感），然后再做数值
比较。序关系运算要求两边都能解析成数字 —— 否则退回按字典序逐字符比较，于是
``"9" > "10"``。JSON 里的阈值可以写成字符串、数字或布尔值。

``then`` — 步骤
--------------------

``then`` 是一个 **有序的步骤列表**。每个步骤是一个对象，包含：

* ``actions`` —— 一起执行的动作数组（每步最多 4 个）。
* ``delay_after`` —— 本步动作完成后等待的秒数。后面还有步骤时，这段等待把两个
  时刻分开；最后一步之后没有步骤，等待只推迟规则的结束。0 表示无延迟。

步骤串行执行：第 0 步在 ``when.delay_sec`` 之后运行，之后每步在前一步的动作和
``delay_after`` 都完成后开始。要推迟第一步（或唯一一步），请设 ``when.delay_sec``；
最后一步末尾的等待不会把它的动作后移。这取代了旧的独立 ``delay`` 动作：等待
现在属于前一个步骤，而不再是单独的动作。旧的扁平 ``then`` （单个动作对象或数组，
可含内联 ``delay`` 动作）固件仍会解析为步骤。

.. code-block:: text

   when(触发) --[when.delay_sec]--> 步骤0动作 --[delay_after]--> 步骤1动作 --> ...
   最后一步： ... 动作 --[delay_after]--> 规则结束

Web 编辑器里，等待时间由该步骤的「动作类型」下拉选择 ``延迟`` 得到（没有独立的
延迟输入框）：延迟总是落在该步骤末尾，一个步骤最多一个。只有最后一步以延迟结尾，
才会出现 ``+ 添加步骤``——正是这段延迟把两个时刻分开。

动作（``type``）
~~~~~~~~~~~~~~~~~~~

**只有** 下列 ``type`` 会被识别（没有 ``scene`` 动作，也不直接调用
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
   * - ``delay``
     - ``delay_sec`` （也接受 ``seconds``）。旧的步骤内等待：写在一个步骤里，
       它会让后续动作先等一等；写在扁平 ``then`` 中，则成为一个步骤边界。
       编辑器会把它改写为 ``delay_after``。

未知 ``then.type`` 记日志并跳过。

执行为 **非阻塞**：延迟由一次性 ``esp_timer`` 回调驱动，回调只把执行任务投递到
专用任务队列——命令绝不在定时器回调内执行。每次重载都会递增一个单调代际计数器，
因此重载前已挂起的延迟任务会被丢弃（避免重载后执行到失效的规则内存）。

活儿跑在哪个任务上会影响你对规则计时：``point.updated`` 的匹配发生在 Hub 事件总线的
消费任务上，也就是说 ``if`` 条件在那上面求值，只有动作交给专用执行任务；时间与
``once`` 规则由第三个任务每秒轮询。

固件里写死的上限：

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - 项目
     - 上限，以及超出后的行为
   * - 文档内规则数
     - 16 条 —— 后面的条目被静默丢弃
   * - 单规则步骤数
     - 5 步 —— 后面的步骤被丢弃（旧扁平写法会记一条警告）
   * - 单步动作数
     - 4 个 —— 后面的动作被丢弃
   * - ``if`` 条件数
     - 4 条 —— 后面的条件被忽略
   * - 点位触发索引
     - 16 个桶 × 每桶 8 条 —— 桶已满的规则 **不会** 被触发，日志里会说明
   * - 挂起中的延迟
     - 8 个定时器槽 —— 全部占用时，下一步 **不再等待** 直接执行，并记一条警告
   * - 排队的动作任务
     - 16 个 —— 队列满则丢弃该任务
   * - 边沿检测缓存
     - 256 个 ``(point_id, parameter)`` —— 超出后，重复上报的值可能再次触发
   * - ``rules.json``
     - 128 KiB —— 导入 body、拉取的 URL、场景导入都用这一个上限

上面每一项上限，连同设定它的代码行，见 :doc:`limits` 。

导入与 MCP
----------

* REST 导入：``POST /api/rules/import``，body 为文档本身或
  ``{"url":"http://…"}``。
* MCP：``rules.list`` / ``rules.put``。

.. warning::

   导入是 **整份替换** ``/data/config/rules.json``—— 不会与设备上已有的规则合并。
   ``minihub_config_save_rules()`` 把你发过去的文档写好再改名盖住正在用的文件，所以
   你没写进 body 的规则在重载后就没了。先 GET 当前文档、在上面追加、再导入结果；
   场景同理。改名这一步也解释了为什么中途失败的导入留下的还是旧规则，而不是一份
   被截断的文件。

相关
----

* 点位模型：:doc:`data_model`
* 场景：:doc:`data_model` （``scenes.json``）
* REST：:doc:`api/host_rest`
* 端到端流程：:doc:`data_flow`
* ``/ws/events`` 事件类型：:doc:`api/host_rest`

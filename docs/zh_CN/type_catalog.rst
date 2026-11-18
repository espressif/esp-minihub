类型目录与 UI
=============

:link_to_translation:`en:[English]`

驱动概览页的是 Host **类型目录**：一组 CSV 表，列出类型、参数名、控件与默认范围。
画一个类型不需要新的 C 结构，所以 Hub 出厂之后仍可加类型。仓库里
``components/minihub_device_widget/model/`` 下的 CSV 编译进固件，改它们要重编；
设备上的运行时表 ``/data/config/devices.csv`` 优先于内嵌副本被读取，改它不必重烧。

能不能控制也读同一份目录：类型提供可写的 ``power`` 参数时，概览卡片才画电源开关、
地图标点才可点击开关 （ ``components/minihub_web/web/js/device_widgets.js`` 里的
``dwHasPowerControl`` ）。目录没列出的类型看点位自己：有 ``power`` 参数，或者
``value`` 是布尔摘要。实例覆盖类型 —— ``props.params.power`` 上 ``visible: false``
或 ``access: "R"`` 就没有开关。

契约的运行时那一半 （一台设备一条 point、值在 ``props.params``）见 :doc:`data_model`。

控件画在两半相交的地方：类型目录给形状，在线 ``props.params`` 给数值，画图的是 CSS，
不是每个参数一张 SVG。

**厂商 / 品类故意不进这些表。** 它们描述的是某一台设备，不是某一类设备，所以写在
点位顶层 ``props`` （见 :doc:`data_model`）：

* ``manufacturer`` — 详情「厂商」
* ``category`` （或 ``product`` / ``product_name``）— 品类展示名；卡片角标
  优先用它。控件、过滤下拉仍用 ``point.type`` 对齐本页目录。
* 不要把厂商、品类、OTA 加进 ``parameters.csv``。

内置设备类型与参数
------------------

权威源码为 ``components/minihub_device_widget/model/*.csv``。内置设备类型、参数与
UI 元素由文档构建自上述 CSV 生成（见下文表格）。

* :download:`devices.csv <../_static/catalog/model/devices.csv>`
* :download:`parameters.csv <../_static/catalog/model/parameters.csv>`
* :download:`ui_elements.csv <../_static/catalog/model/ui_elements.csv>`
* :download:`services.csv <../_static/catalog/model/services.csv>` —— 仅供参考（RainMaker 系统服务），不属于控件注册表
* :download:`catalog.zip <../_static/catalog.zip>`

内置设备类型
^^^^^^^^^^^^

.. minihub-catalog:: devices

插件 ``devices.json`` **只需声明本台设备真正用到的子集**；不必把类型上全部
参数都挂上。JS 演示：

* ``examples/plugins/color_bulb_js`` → ``type: light`` （power / brightness / hue /
  saturation；cct 用 ``visible: false`` 藏起）
* ``examples/plugins/meter_plug_js`` → ``type: switch`` （power 可写；电压电流功率只读）

目录内已内置以下传感器类型：``sensor_contact``、``sensor_motion``、
``sensor_occupancy``、``sensor_tamper``、``sensor_water``、``sensor_smoke``、
``sensor_illuminance``、``sensor_temp_humidity``、``sensor_air_quality``。Zigbee / Matter / MQTT 等
适配器也可登记 ``unknown`` 等其它 type 字符串：它们能进点位表与规则。类型不在
目录里时，设备详情仍会画参数行 —— 参数名在参数表里有条目就照它的标签和控件画，
没有就退化成一行只读文本、用原名当标签 —— 只是没有类型级的参数次序、默认范围和
头像，浏览器控制台会记一条 ``type not in catalog``。要出完整目录 UI，先把类型写进
CSV。

内置参数与控件
^^^^^^^^^^^^^^

目录接受的 ``ui_element`` 就是
``components/minihub_device_widget/model/ui_elements.csv`` 里的那些 id ：目前为
``ui.toggle`` / ``ui.slider`` / ``ui.dropdown`` /
``ui.text`` / ``ui.button`` / ``ui.hidden``，Web 侧对应 toggle / slider / dropdown /
text / button / hidden；下表列出的是当前各行实际用到的。另有两个旧名 ``ui.hue-slider`` / ``ui.hue-circle`` 只在
目录里仍被接受，都算 slider；插件 ``devices.json`` 不认它们，也不认封闭集合之外的
任何名字。色相 / 饱和度 / 色温都是 **slider**，彩虹或色带靠 CSS，不是单独控件类型。

.. figure:: ../_static/data_model/widget_styles.svg
   :alt: toggle、slider、dropdown、text、button、hidden 这些参数控件各自的形状

   每个 ``ui_element`` 画成什么样。

.. minihub-catalog:: parameters

``ui.hidden`` = 类型级永不画行。实例 ``visible: false`` = **这台** 设备藏参数，
但仍留在 ``props.params``。只读传感器用实例 ``access: "R"`` （仍显示，无拖动）。
控件形状与点位数值如何合成一行，见 :doc:`data_model` 。

UI 元素
++++++++

上面的闭合集合，逐个说明：

.. minihub-catalog:: ui_elements

``wifi_signal`` （WiFi RSSI，dBm）这类只读遥测在目录里是一条 ``ui.text`` 参数。
ESPHome、Tasmota 这类 MQTT 适配器把它当作**实例**参数挂到设备点位上，而不是内置
类型的某一列，并写 ``access: "R"``；详情面板于是只显示数值、不提供任何控件。要加
任意这类只读传感器：在 ``parameters.csv`` 写一行 ``ui_element: ui.text``，并在实例
条目上设 ``access: "R"``。ESPHome 从它的 ``sensor.wifi_signal`` 实体取这个值；一台
设备一条点位的注册契约见 :doc:`data_model` 。

图标与图案
----------

设备 **头像** 是 SVG，源文件在
``components/minihub_device_widget/images/devices/``。构建把它们打进设备 storage 分区的
``/data/images``，Web 经 ``/img/devices/`` 下与类型同名的文件拉取；
``GET /api/device_widgets`` 也会带上 URL。上表的图标列就是这些文件。

参数行 **不是** 每参数一张 SVG：交互控件是 ``device_widgets.js`` /
``device_widgets.css`` 画的 （开关、滑杆、下拉、文本、按钮），形状见
:doc:`data_model`。可选参数图标目录 ``images/parameters/`` 目前可空；有文件时
命名约定见 ``components/minihub_device_widget/README_CN.md`` 的「图标」一节。

CSV 列说明
----------

两层。Core 不拥有任何一份 CSV。

.. code-block:: text

   Host 类型目录                    插件实例（这一台）
   model/devices.csv                devices.json
   model/parameters.csv             -> 只登记一条 point
          |                                |
          v                                v
   GET /api/device_widgets          GET /api/points（props.params）
          \                              /
           界面 = 目录 ∩ 在线参数

``devices.csv`` 列：``device_id``、``parameters`` （逗号分隔参数名）、
``primary_param``、``mandatory_params``、``image`` （``images/devices/``
下的类图标）。上传时这五个表头都必须在，``image`` 的格子可以留空。

``primary_param`` 指出该类型用哪个参数概括一台设备，适配器可以拿它当点位的
紧凑 ``value``。生成的扁平注册表把这一列重复写在该类型的每一行上，
``minihub_device_widget_get()`` 则按类型只暴露一次；类型没声明时它是空的。
非空时它必须是该类型 ``parameters`` 里的一项：生成器和固件都会拒绝类型没列出的名字。

``parameters.csv`` 列：``parameter``、``label_zh``、``label_en``、``data_type``、
``properties``、``min`` / ``max`` / ``step`` / ``unit``、``ui_element``、
``options`` （下拉 JSON 数组；每项写 ``labels`` 映射，不再写平铺的 ``label``）、
``description``、``visible``、``css``、``image`` —— 上传时这十五个表头是必需的；
``requires`` 可选。``visible`` 必须存在但值不被读取：界面只按实例
``props.params.名称.visible`` 隐藏参数。参数 ``image`` 会进扁平表，也是固件解析控件
图标时的第一个候选，其次才是 ``<device>_<parameter>.svg`` 与 ``<parameter>.svg``；
但 ``/api/device_widgets`` 对它恒回 ``""``：控制台的参数行只认插件实例自己写的
``icon``，控件本身由 CSS 画。

上传的限制：单文件 128 KiB、最多 24 列、256 行；表头大小写不敏感 （参数名不是）。

两列显示名是控制台双语的来源。``/api/device_widgets`` 每个参数只下发一个
``label``，由 ``lang`` 查询参数从该行的 ``labels`` 映射里挑：``lang`` 传了 Hub
不认识的值就按 ``zh`` 出，完全不传 ``lang`` 时看 ``Accept-Language``，都没有就
``en``。加一种语言要动五个已知的地方：
CSV 新加一列、生成器的 ``LABEL_COLS``、扁平化那一侧的列集合（``PARAM_REQ_COLS``、
``dw_param_cols_t``、``dw_build_style``）、``dw_request_lang()`` 认识的语言码，
以及控制台自己的语言集（``i18n.js`` 与开关里的选项）；而取用显示名的那些点
都不用动——条目结构体、API 形状、卡片与规则渲染都保持原样。

``components/minihub_device_widget/tools/check_catalog_i18n.py`` 随生成器一起跑，
按下面三级判定：

* **错误** — ``label_zh`` / ``label_en`` 列缺失、``label_zh`` 单元格为空、
  ``options`` 项没有 ``labels`` 映射、控制台两份文案文件 key 集合不一致、
  或代码取用的 key 在任何文案文件里都没有定义；
* **警告** — ``label_en`` 单元格为空，或 ``label_en`` 与 ``label_zh`` 相同。
  专名（``PM2.5``、``HSV``、``CCT``）中英本来就一样，所以相同只警告不失败；
* **运行时回落** — 该行没提供的语言，先回落 ``label_zh``，再回落参数名。
  警告只会退化成中文，不会拦住构建，也不会弄坏页面。

``requires`` 是可写控件的前提。默认写一个参数名：``power`` 与
``{"all": ["power"]}`` 相同，该参数明确关闭时控件禁用。多个参数用对象：

* ``{"all": ["power", "child_lock"]}`` — 相与，列出的参数都要开着。有一个明确关闭就禁用。
* ``{"any": ["power", "indicator"]}`` — 相或，至少一个开着即可。全部明确关闭才禁用。
* ``{"not": ["child_lock"]}`` — 这些参数不能开着。有一个明确打开就禁用。``not`` 可以单独写，也可以和 ``all`` 或 ``any`` 写在一起 （二者不能同时出现），例如 ``{"all": ["power"], "not": ["child_lock"]}``。

参数不存在，或取值不是开/关，既不算开也不算关。留空表示没有前提。只读参数不会因此被禁用。
``power`` 和 ``indicator`` 留空，关灯后仍可操作。插件 ``devices.json`` 可以写
参数名或 ``all`` / ``any`` / ``not`` 对象，覆盖这一台设备的目录默认值。``parameters.csv``
里的对象要写成带引号的 JSON 单元格。同一个名字不能出现两次，也不能同时出现在
``all`` 或 ``any`` 和 ``not`` 里。

类型目录远程更新—— Web 控制台没有对应页面，走 REST 开发路由（仍需
``CONFIG_MINIHUB_WEB_UI`` 才能提供文件）：
``GET`` / ``PUT /api/dev/catalog/devices.csv`` 与
``GET`` / ``PUT /api/dev/catalog/parameters.csv``。两份都齐后才会扁平化并
reload；只传一份时 PUT 仍然成功，但回 ``rebuilt: false`` 并带一条提示。

每一步失败的粒度各不相同：

* **PUT** 是整份成或整份败。``device_id`` 或 ``parameter`` 重复、
  ``ui_element`` 不认识、``requires`` 写法不对，都会让这次上传直接 **400**，
  磁盘上的东西一点没动。
* **扁平化** 同样全有或全无：设备行里引用了一个在 ``parameters.csv`` 没有条目的
  参数，整次扁平化就中止。
* 真正会跳行的是读扁平表那一步：字段错位、或 style JSON 里没有可用
  ``widget`` 的行被丢弃，并在日志里计数。若一行都不剩，整个文件被判为不可用。
* **被判不可用的扁平文件仍会被重写。** reload 会先继续用旧表对外服务，随后用
  固件内嵌的注册表重新写出 ``/data/config/devices.csv`` —— 所以手改这个文件的人，
  在一次解析失败后就会发现改动没了；开机时读到坏文件也是同样的重写。

见 :doc:`data_flow`。

开发时怎么用
------------

1. 已有类型上加一台设备 （最常见）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

不改 Host CSV。在插件里写 ``devices.json``，参数名必须已在目录中：

.. code-block:: JSON

   [
     {
       "device": "kitchen_lamp",
       "type": "light",
       "params": [
         { "name": "power", "init": false, "access": "RW" },
         { "name": "brightness", "init": "80", "access": "RW", "max": 80, "requires": "power" },
         { "name": "hue", "init": "0", "access": "RW", "requires": { "all": ["power"] } },
         { "name": "saturation", "init": "100", "access": "RW", "requires": { "any": ["power"] } },
         { "name": "cct", "init": "4000", "visible": false, "access": "RW", "requires": "power" }
       ]
     },
     {
       "device": "living_lamp",
       "type": "light",
       "params": [
         { "name": "power", "init": true, "access": "RW" },
         { "name": "brightness", "init": "40", "access": "RW", "max": 80, "requires": "power" }
       ]
     }
   ]

登记时（数组每个元素一个点）：

* ``point_id`` = ``你的协议前缀.device`` （如 ``sim.kitchen_lamp``）
* ``type`` = ``devices.json`` 的 ``type`` （须是目录里的 ``device_id``）
* 把 ``params`` 填进 ``props.params`` （``init`` → ``value``；可写
  ``access`` / ``visible`` / ``min`` / ``max`` / ``step`` / ``unit`` / ``requires``）
* 展示覆盖项：参数级 ``icon`` （必须写成 ``./images/...``，且该文件已在
  ``components/minihub_device_widget/images/`` 下—— 插件不能自带图）、
  ``ui_element`` （取 ``ui.toggle`` / ``ui.slider`` / ``ui.dropdown`` / ``ui.text`` /
  ``ui.button`` / ``ui.hidden`` 之一）、``css`` （纯类名）；设备级 ``icon``。不写则用该 ``type``
  在目录里的默认样式。``generate_devices_property_from_json.py`` 会拦掉不存在的
  图标、封闭集合之外的控件名和不安全的 ``css``。

单对象（非数组）仍可用于只登记一台的插件。

.. only:: not hide_l3

   ELF：改 JSON 后重新生成 ``device_desc.gen.h``，不要手改头文件。ELF 源码用
   ``tools/new_store_module.py --tier l3`` 生成。

2. 增加全新设备类型
^^^^^^^^^^^^^^^^^^^

1. 改 Host ``components/minihub_device_widget/model/devices.csv`` 与
   ``parameters.csv`` （需要时加 ``images/devices/<id>.svg``）
2. ``python3 components/minihub_device_widget/tools/gen_device_widget.py --check``
3. 重编 Host 固件

或不重烧（REST，无 Web 页面）：``PUT /api/dev/catalog/devices.csv`` 与
``PUT /api/dev/catalog/parameters.csv`` （两份都齐才扁平化并 reload；
``python3 tools/dev_sync.py --open``）。

.. warning::

   这两个 PUT 替换的是设备上 **运行时** 那份目录，而且一次只换一个文件。两份都到位
   之后注册表才会重建—— 返回里 ``"rebuilt": false`` 并带 ``hint`` 就说明这一对还不齐，
   控制台会继续用旧目录渲染，直到你把另一份发过去。格式不对的 CSV 是被拒绝而不是被
   加载，所以在用的目录不会出现写了一半的状态。要退回 ``DELETE /api/dev/catalog/<file>.csv`` ：
   它删掉存下来的那份，下一次 reload 用固件里内置的目录重新播种。这个回退才是恢复手段——
   光重启并不会撤销一次传坏了的上传。

若是 **Zigbee** 类别，还要扩展
``components/minihub_zigbee/config/zigbee_cluster_map.json`` （或 overlay
``/data/config/zigbee_cluster_map.json``），使入网访谈 / 上报 / 命令接到
``props.params``，无需改 ``zigbee_adapter.c``。见 :doc:`zigbee`。

3. 读状态与下发控制
^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - 接口
     - 用途
   * - ``GET /api/points``
     - 实例：点位 + ``props.params``
   * - ``GET /api/device_widgets``
     - 类型目录：标签 / 控件 / 默认范围 / 头像 URL
   * - ``POST /api/devices/control``
     - 概览页：``device`` = **类型** （如 ``light``），加 ``parameter``、
       ``value``；UI 同时带 ``point_id`` 指定实例
   * - ``POST /api/command``
     - 自动化：明确 ``point_id`` + ``cmd``；可选 ``params.parameter``
   * - ``turn_on`` / ``turn_off`` / ``toggle``
     - 开关类常用命令；其它 ``cmd`` 由适配器定义

规则若写了 ``parameter``，比较的是 ``props.params.名称.value``，不是摘要
``value``。详见 :doc:`data_model` 与 :doc:`api/host_rest`。

4. 谁改什么 （速查）
^^^^^^^^^^^^^^^^^^^^

* **新类型 / 默认文案控件范围** → Host CSV （或远程 catalog PUT）
* **已有类型上的新设备** → 插件 ``devices.json``
* **只藏/收紧这一台** → 实例 ``visible`` / ``access`` / ``min`` / ``max``
* **不要** 为类型加 Core C 结构

相关
----

* 点位与 Hub 配置：:doc:`data_model`
* REST 示例：:doc:`api/host_rest`
* 端到端流程：:doc:`data_flow`
* 类型目录组件：``components/minihub_device_widget/README_CN.md``

.. only:: not hide_l3

   * ELF 插件：:doc:`plugins/develop_elf`

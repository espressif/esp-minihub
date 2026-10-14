类型目录与 UI
=============

:link_to_translation:`en:[English]`

Host **类型目录** 决定设备页怎么画：有哪些类型、参数标签、控件与默认范围。
运行时点位契约 （一台设备一条 point、``props.params``）见 :doc:`data_model`。

界面 = 类型目录 ∩ 在线 ``props.params`` （CSS 控件，不是每个参数一张 SVG）。

**厂商 / 品类不是 CSV 参数。** 写在点位顶层 ``props`` （见 :doc:`data_model`）：

* ``manufacturer`` — 详情「厂商」
* ``category`` （或 ``product`` / ``product_name``）— 品类展示名；卡片角标
  优先用它。控件、过滤下拉仍用 ``point.type`` 对齐本页目录。
* 不要把厂商、品类、OTA 加进 ``parameters.csv``。

内置设备类型与参数
------------------

权威源码：``components/minihub_device_widget/model/*.csv``。完整对照表见
``components/minihub_device_widget/STANDARD_TYPES_CN.md``。

* :download:`devices.csv <../_static/catalog/model/devices.csv>`
* :download:`parameters.csv <../_static/catalog/model/parameters.csv>`
* :download:`ui_elements.csv <../_static/catalog/model/ui_elements.csv>`
* :download:`services.csv <../_static/catalog/model/services.csv>`
* :download:`STANDARD_TYPES_CN.md <../_static/catalog/STANDARD_TYPES_CN.md>`
* :download:`catalog.zip <../_static/catalog.zip>`

内置设备类型
^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 12 28 12 12 36

   * - 类型 id
     - 参数 （目录里列出的全集）
     - 主参数
     - 必选
     - 头像
   * - ``light``
     - power, brightness, cct, hue, saturation, intensity
     - power
     - power
     - 灯泡图标 （见下）
   * - ``switch``
     - power, voltage_v, current_a, power_w, energy_wh, temperature, fault, indicator
     - power
     - power
     - 插座/开关图标 （见下）
   * - ``speaker``
     - power, volume
     - volume
     - volume
     - 音箱图标 （见下）
   * - ``sensor_air_quality``
     - co2, pm25, pressure_hpa, battery
     - co2
     - co2
     - 空气质量图标 （见下）
   * - ``sensor_contact``
     - contact, battery
     - contact
     - contact
     - 门磁图标 （见下）
   * - ``sensor_illuminance``
     - illuminance, battery
     - illuminance
     - illuminance
     - 光照度图标 （见下）
   * - ``sensor_motion``
     - motion, battery
     - motion
     - motion
     - 人体移动图标 （见下）
   * - ``sensor_occupancy``
     - occupancy, battery
     - occupancy
     - occupancy
     - 有人图标 （见下）
   * - ``sensor_smoke``
     - smoke, battery
     - smoke
     - smoke
     - 烟雾图标 （见下）
   * - ``sensor_tamper``
     - tamper, battery
     - tamper
     - tamper
     - 防拆图标 （见下）
   * - ``sensor_temp_humidity``
     - temperature, humidity, battery
     - temperature
     - temperature
     - 温湿度图标 （见下）
   * - ``sensor_water``
     - leak, battery
     - leak
     - leak
     - 漏水图标 （见下）

插件 ``devices.json`` **只需声明本台设备真正用到的子集**；不必把类型上全部
参数都挂上。JS 演示：

* ``examples/plugins/color_bulb_js`` → ``type: light`` （power / brightness / hue /
  saturation；cct 用 ``visible: false`` 藏起）
* ``examples/plugins/meter_plug_js`` → ``type: switch`` （power 可写；电压电流功率只读）

目录内已内置以下传感器类型：``sensor_contact``、``sensor_motion``、
``sensor_occupancy``、``sensor_tamper``、``sensor_water``、``sensor_smoke``、
``sensor_illuminance``、``sensor_temp_humidity``、``sensor_air_quality``。Zigbee / Matter / MQTT 等
适配器也可登记 ``unknown`` 等其它 type 字符串：它们能进点位表与规则，但若类型
不在目录里，设备页会显示 **类型未登记**，没有标准控件卡。要出标准 UI，先把
类型写进 CSV。

内置参数与控件
^^^^^^^^^^^^^^

封闭 ``ui_element``：``ui.toggle`` / ``ui.slider`` / ``ui.dropdown`` /
``ui.text`` / ``ui.hidden``。Web 侧对应 toggle / slider / dropdown / text /
hidden。色相 / 饱和度 / 色温都是 **slider**，彩虹或色带靠 CSS，不是单独控件类型。

.. list-table::
   :header-rows: 1
   :widths: 14 12 10 18 14 32

   * - 参数
     - 标签
     - 类型
     - 默认范围
     - UI
     - 用在
   * - ``power``
     - 电源
     - Bool
     - —
     - toggle
     - light, switch, speaker
   * - ``brightness``
     - 亮度
     - Int
     - 0–100 %, step 1
     - slider
     - light
   * - ``cct``
     - 色温
     - Int
     - 2700–6500 K, step 100
     - slider
     - light
   * - ``hue``
     - 颜色
     - Int
     - 0–360 °, step 1
     - slider （彩虹轨）
     - light
   * - ``saturation``
     - 饱和度
     - Int
     - 0–100 %, step 1
     - slider
     - light
   * - ``intensity``
     - 强度
     - Int
     - 0–100 %, step 1
     - slider
     - light
   * - ``volume``
     - 音量
     - Int
     - 0–100, step 1
     - slider（依赖 power）
     - speaker
   * - ``voltage_v``
     - 电压
     - Float
     - 0–250 V
     - text（只读计量）
     - switch
   * - ``current_a``
     - 电流
     - Float
     - 0–16 A
     - text（只读计量）
     - switch
   * - ``power_w``
     - 功率
     - Float
     - 0–4000 W
     - text（只读计量）
     - switch （有功功率；不是开关 ``power``）
   * - ``energy_wh``
     - 电量
     - Float
     - 0–999999 Wh
     - text（只读计量）
     - switch
   * - ``name``
     - 名称
     - String
     - —
     - text
     - 元数据；无设备默认挂它 → 一般不画控件
   * - ``temperature``
     - 温度
     - Float
     - -40–125 °C, step 0.1
     - text（只读）
     - switch, sensor_temp_humidity
   * - ``humidity``
     - 湿度
     - Float
     - 0–100 %, step 0.1
     - text（只读）
     - sensor_temp_humidity
   * - ``illuminance``
     - 光照度
     - Float
     - 0–100000 lx, step 1
     - text（只读）
     - sensor_illuminance
   * - ``co2``
     - 二氧化碳
     - Int
     - 0–5000 ppm, step 1
     - text（只读）
     - sensor_air_quality
   * - ``pm25``
     - PM2.5
     - Float
     - 0–1000 µg/m³, step 0.1
     - text（只读）
     - sensor_air_quality
   * - ``pressure_hpa``
     - 气压
     - Float
     - 300–1100 hPa, step 0.1
     - text（只读）
     - sensor_air_quality
   * - ``battery``
     - 电量
     - Int
     - 0–100 %, step 1
     - text（只读）
     - sensor_* （所有）
   * - ``contact``
     - 门磁
     - Bool
     - —
     - text（只读）
     - sensor_contact
   * - ``motion``
     - 人体移动
     - Bool
     - —
     - text（只读）
     - sensor_motion
   * - ``leak``
     - 漏水
     - Bool
     - —
     - text（只读）
     - sensor_water
   * - ``smoke``
     - 烟雾
     - Bool
     - —
     - text（只读）
     - sensor_smoke
   * - ``occupancy``
     - 有人
     - Bool
     - —
     - text（只读）
     - sensor_occupancy
   * - ``tamper``
     - 防拆
     - Bool
     - —
     - text（只读）
     - sensor_tamper

``ui.hidden`` = 类型级永不画行。实例 ``visible: false`` = **这台** 设备藏参数，
但仍留在 ``props.params``。只读传感器用实例 ``access: "R"`` （仍显示，无拖动）。

图标与图案
----------

设备 **头像** 是 SVG，源文件在
``components/minihub_device_widget/images/devices/``。构建打进设备
``/data/images``，Web 经 ``GET /img/devices/<name>`` 拉取；
``GET /api/device_widgets`` 也会带上 URL。

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - 类型
     - 文件
     - 预览
   * - ``light``
     - ``images/devices/light.svg``
     - .. image:: ../_static/catalog/images/devices/light.svg
          :alt: light device icon
          :width: 64px
   * - ``switch``
     - ``images/devices/switch.svg``
     - .. image:: ../_static/catalog/images/devices/switch.svg
          :alt: switch device icon
          :width: 64px
   * - ``speaker``
     - ``images/devices/speaker.svg``
     - .. image:: ../_static/catalog/images/devices/speaker.svg
          :alt: speaker device icon
          :width: 64px
   * - ``sensor_air_quality``
     - ``images/devices/sensor_air_quality.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_air_quality.svg
          :alt: air quality sensor icon
          :width: 64px
   * - ``sensor_contact``
     - ``images/devices/sensor_contact.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_contact.svg
          :alt: contact sensor icon
          :width: 64px
   * - ``sensor_illuminance``
     - ``images/devices/sensor_illuminance.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_illuminance.svg
          :alt: illuminance sensor icon
          :width: 64px
   * - ``sensor_motion``
     - ``images/devices/sensor_motion.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_motion.svg
          :alt: motion sensor icon
          :width: 64px
   * - ``sensor_occupancy``
     - ``images/devices/sensor_occupancy.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_occupancy.svg
          :alt: occupancy sensor icon
          :width: 64px
   * - ``sensor_smoke``
     - ``images/devices/sensor_smoke.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_smoke.svg
          :alt: smoke sensor icon
          :width: 64px
   * - ``sensor_tamper``
     - ``images/devices/sensor_tamper.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_tamper.svg
          :alt: tamper sensor icon
          :width: 64px
   * - ``sensor_temp_humidity``
     - ``images/devices/sensor_temp_humidity.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_temp_humidity.svg
          :alt: temperature / humidity sensor icon
          :width: 64px
   * - ``sensor_water``
     - ``images/devices/sensor_water.svg``
     - .. image:: ../_static/catalog/images/devices/sensor_water.svg
          :alt: water-leak sensor icon
          :width: 64px

参数行 **不是** 每参数一张 SVG：交互控件是 ``device_widgets.js`` /
``device_widgets.css`` 画的 （开关、滑杆、下拉、文本）。可选参数图标目录
``images/parameters/`` 目前可空；有文件时命名约定见
``components/minihub_device_widget/README_CN.md`` 的「图标」一节。

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
``primary_param``、``mandatory_params``、可选 ``image`` （``images/devices/``
下的类图标）。

``parameters.csv`` 列：``parameter``、``label_zh``、``label_en``、``data_type``、
``properties``、``min`` / ``max`` / ``step`` / ``unit``、``ui_element``、
``options`` （下拉 JSON 数组；每项写 ``labels`` 映射，不再写平铺的 ``label``）、
``description``、``css``、可选 ``requires``。
遗留的 ``visible`` 表头会被忽略；隐藏用实例 ``visible: false``。参数
``image`` 列当前不用 （控件走 CSS）。

两列显示名是控制台双语的来源。``/api/device_widgets`` 每个参数只下发一个
``label``，由 ``lang`` 查询参数（``zh`` / ``en``，缺省回落 ``Accept-Language``，
再回落 ``zh``）从该行的 ``labels`` 映射里挑。加一种语言要动五个已知的地方：
CSV 新加一列、生成器的 ``LABEL_COLS``、扁平化那一侧的列集合（``PARAM_REQ_COLS``、
``dw_param_cols_t``、``dw_build_style``）、``dw_request_lang()`` 认识的语言码，
以及控制台自己的语言集（``i18n.js`` 与开关里的选项）；而取用显示名的那些点
都不用动——条目结构体、API 形状、卡片与规则渲染都保持原样。

``tools/check_catalog_i18n.py`` 随生成器一起跑，按下面三级判定，让缺翻译在
构建期暴露，而不是几个月后才被发现：

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

类型目录远程更新 （需 ``CONFIG_MINIHUB_WEB_UI``）：
``GET`` / ``PUT /api/dev/catalog/devices.csv`` 与
``GET`` / ``PUT /api/dev/catalog/parameters.csv``。两份都齐后才会扁平化并
reload。reload 为原子且可容错 （坏行被跳过、无法解析的文件保留旧表、
缺失 / 损坏文件从内嵌副本自愈）；见 :doc:`architecture_overview`。

开发时怎么用
------------

1. 已有类型上加一台设备 （最常见）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

不改 Host CSV。在插件里写 ``devices.json``，参数名必须已在目录中：

.. code-block:: json

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
* 展示覆盖项：参数级 ``icon`` （只能引用已随固件发布的 ``./images/...`` 图标）、
  ``ui_element`` （取 ``ui.toggle`` / ``ui.slider`` / ``ui.dropdown`` / ``ui.text`` /
  ``ui.hidden`` 之一）、``css`` （纯类名）；设备级 ``icon``。不写则用该 ``type``
  在目录里的默认样式。``generate_devices_property_from_json.py`` 会拦掉不存在的
  图标、封闭集合之外的控件名和不安全的 ``css``。

单对象（非数组）仍可用于只登记一台的插件。

.. only:: not hide_l3

   L3：改 JSON 后重新生成 ``device_desc.gen.h``，不要手改头文件。演示目录：
   ``examples/plugins/color_bulb``、``examples/plugins/meter_plug``。

2. 增加全新设备类型
^^^^^^^^^^^^^^^^^^^

1. 改 Host ``components/minihub_device_widget/model/devices.csv`` 与
   ``parameters.csv`` （需要时加 ``images/devices/<id>.svg``）
2. ``python3 components/minihub_device_widget/tools/gen_device_widget.py --check``
3. 重编 Host 固件

或不重烧：``PUT /api/dev/catalog/devices.csv`` 与
``PUT /api/dev/catalog/parameters.csv`` （两份都齐才扁平化并 reload；
``python3 tools/dev_sync.py --open``）。

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
     - 设备页：``device`` = **类型** （如 ``light``），加 ``parameter``、
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
* 端到端流程：:doc:`architecture_overview`
* 类型目录组件：``components/minihub_device_widget/README_CN.md``

.. only:: not hide_l3

   * L3 插件：:doc:`plugins/develop_l3`

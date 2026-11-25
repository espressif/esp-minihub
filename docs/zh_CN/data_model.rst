数据模型
========

:link_to_translation:`en:[English]`

**运行时点位模型** 落在 ``adapters.json`` 与场景里。规则单独成章，见 :doc:`rules`；
概览页用的类型、控件与图标见 :doc:`type_catalog`。

运行时点位模型概览
------------------

Core 并不知道「灯」是什么。它 **按点位工作**：类型只是一个名字，参数以 JSON 挂在
点位上，所以新增设备类型不必改 Core：

* **一台物理设备对应一条** ``minihub_point_t`` （Matter 按 endpoint，
  见 :doc:`matter`）
* ``point_id`` = ``协议.设备名`` （如 ``sim.js_kitchen``、
  ``zigbee.zb_7d4b``、``thread.matter_n2_ep1``）
* 实时参数都在该点的 ``props.params.<参数名>`` 里
* ``type`` 是字符串 （``light`` / ``switch`` / …），**不是** Core 里的封闭枚举
* **怎么画 UI** 来自 Host **类型目录** （CSV），与协议无关 —— 见
  :doc:`type_catalog`。目录是数据，但内嵌那份编译进固件：改仓库里
  ``components/minihub_device_widget/model/`` 下的 CSV 要重编，改设备上的
  ``/data/config/devices.csv`` 不必重烧。

.. figure:: ../_static/data_model/point_lifecycle.zh_CN.svg
   :alt: 状态从设备经适配器写进 props.params，用户通过概览页、规则与场景读取，命令按原路回到设备
   :width: 100%

   设备状态经适配器写入 ``props.params``，由概览页、规则与场景读取；命令沿同一路径
   返回设备。概览页、规则与场景共用同一份 ``props.params``。

类型目录里有该参数、且实时点位的 ``props.params`` 里也有，UI 才会显示它。

**不要** 按参数再拆点 （错误示例：``sim.js_kitchen.brightness``）。

活动点示例 （``GET /api/points``）：

.. code-block:: JSON

   {
     "point_id": "sim.js_kitchen",
     "type": "light",
     "adapter": "color_bulb_js",
     "value": "on",
     "online": true,
     "props": {
       "manufacturer": "Espressif",
       "category": "Color Bulb",
       "model": "Bulb-42",
       "sw_version": "1.2.0",
       "hw_version": "3",
       "ota": { "available": false },
       "params": {
         "power": { "value": true, "access": "RW" },
         "brightness": { "value": 80, "access": "RW", "max": 80 }
       }
     }
   }

设备身份（顶层 ``props``，不是目录 ``params``）
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

在注册点位时写入 Host（或之后 ``merge_props``）。总览卡片不展示这些身份字段
（开关 / 计量仍走 ``props.params``）。**设备详情** 里厂商 / 型号 / 版本 / OTA
缺键显示为空；**品类** 行总会有文案（``category`` 或 ``TYPE_LABEL[type]``）。

================  ============================================================
键                含义与谁来写
================  ============================================================
``manufacturer``  厂商 / 品牌。详情「厂商」。Zigbee：Host 从 Basic 0x0004
                  读取。BLE Mesh：Composition CID（``0x02E5`` →
                  ``Espressif``，否则 ``0xNNNN``）。Matter：Basic
                  Information VendorName。自建点的插件在 ``register`` 时写入。
``category``      品类展示名（详情标题旁、卡片角标）。控件仍按 ``type``
                  对齐类型目录。缺省时 UI 用 ``TYPE_LABEL[type]``
                  （如 ``light`` →「灯」）。别名：``product`` /
                  ``product_name``。
``model``         型号。Zigbee：Basic 0x0005。BLE Mesh：Composition PID
                  （``0xNNNN``）。Matter：Basic Information ProductName。
``ieee``          协议层地址（如 Zigbee ``0x`` + 16 位十六进制）。Host
                  射频适配器写入；详情不单独成行，可给插件匹配用。
                  BLE Mesh 用 ``uuid`` （Device UUID 十六进制）。
                  Matter 用 ``node_id`` （十进制）。
``sw_version``    软件 / 应用版本。Zigbee：Basic Application Version。
                  BLE Mesh：Composition VID（``0xNNNN``）。Matter：Basic
                  Information SoftwareVersionString。
``hw_version``    硬件版本。Zigbee：Basic HW Version。
``owner``         插件关联后，若 ``adapters.json`` 里该插件槽为
                  ``enabled``，总览把卡片从射频分组挪到该适配器分组。
                  **不要** 改 ``point.adapter`` （命令仍走 Zigbee 等 Host）。
``ota``           对象：``available`` （布尔），可选 ``version`` /
                  ``url`` / ``file`` / ``status``。**仅商店插件** 写入。
                  ``POST /api/devices/ota`` 优先把 ``props.ota.file`` 当
                  ``path``，否则用 ``url``。Host 内置适配器不得伪造可升级状态。
================  ============================================================

内置适配器只填 **协议能确定** 的字段（如 Zigbee Basic 的 ManufacturerName /
ModelIdentifier / ApplicationVersion / HWVersion；BLE Mesh Composition 的
CID/PID/VID 与 ``uuid``；Matter Basic Information 与 ``node_id``）。品类名、OTA、``owner``
由插件通过 ``minihub_point_register`` / ``minihub_point_merge_props`` 提供。
演示 ``color_bulb_js`` / ``meter_plug_js`` 在登记时写入
``manufacturer=Espressif`` 与产品 ``category``。

只有 value 字符串或 ``props`` JSON 真的变了，``minihub_point_set`` 才发
``point.updated`` （WebSocket ``GET /ws/events``）；内容相同的写入是静默的 no-op。
落在该点位防抖窗口内的多次写入会被合成一次收尾发布 （见
:doc:`api/host_rest` 的 ``GET /ws/events``）。merge 系列保留其他顶层键，而给
``minihub_point_set`` 传 ``props`` 会整体替换那个对象。容量上限见
:doc:`api/minihub_core`。

发布实时参数的适配器应调用 ``minihub_point_merge_params`` （或
``minihub_point_merge_props``），以免冲掉同一点上的其他顶层键
（``ota``、``ieee``、``uuid``、``node_id``、``manufacturer``、``category``、``model``、
``sw_version``、``hw_version``、``owner`` 等）。插件在 **同一** ``point_id`` 上补充字段，不得为同一
物理设备再登记第二个点。

支持的设备类型与图标
--------------------

下表在构建时读 ``components/minihub_device_widget/model/devices.csv``，列出的就是
固件当前带的类型。图标优先用插件自带的 ``props.icon``，其次用目录的 ``image``
（``/img/devices/`` 下的 SVG），再其次用 ``/img/devices/<type>.svg``；文件缺失时
控制台画 ``app.js`` 里 ``TYPE_ICON`` 的字形，没有就取类型首字母。``images/devices/``
里另有几个只给总览专用面板的图标（空调、窗帘、电视等），它们不在类型目录里。

.. minihub-catalog:: devices

类型目录里没有的 ``type`` 仍能登记点位、参与规则。设备详情照本台点位上报的参数画
行，缺的只是类型级的次序、默认范围和头像。

参数与控件样式
--------------

下表由 ``parameters.csv`` 生成：一行一个参数，给出默认范围、目录为它选的控件，以及
哪些类型用到它。

.. minihub-catalog:: parameters

.. figure:: ../_static/data_model/widget_styles.svg
   :alt: toggle、slider、dropdown、text、button、hidden 这些参数控件各自的形状
   :width: 100%

   控件由 ``device_widgets.css`` 画，参数行没有图标；``ui.text`` 只读时只显示数值。

一行参数怎么画，由四件事决定：

* **形状** 来自目录的 ``ui_element``。``hue`` / ``saturation`` / ``cct`` 滑杆的色带
  来自按参数名推断的 CSS tone，不是另一种控件。
* **数值** 来自在线点位的 ``props.params``；不在里面的参数不画行。
* **可否操作** 看实例的 ``access``：``R`` 只显示，``RW`` 可改；``requires`` 不满足时变灰。
* **卡片上的电源开关** 同样来自目录：类型要提供可写的 ``power`` 参数。地图标点用
  同一个判断；实例 ``power`` 上 ``visible: false`` 或 ``access: "R"`` 就没有开关。

参数怎么被读写
--------------

``props.params`` 只有一个写入口：适配器或插件上报。其余都是读者，或者发出命令。

.. list-table::
   :header-rows: 1
   :widths: 16 40 44

   * - 谁
     - 读
     - 写
   * - 概览页
     - ``GET /api/device_widgets`` 与 ``props.params`` 的交集
     - 控件变更 → ``POST /api/devices/control``
   * - 规则
     - ``when.parameter`` 指的 ``props.params`` 项
     - ``then`` 的 ``command`` 动作
   * - 场景
     - —
     - ``actions[].params`` 原样交给适配器
   * - REST / MCP
     - ``GET /api/points``
     - ``POST /api/command``

命令生效后，适配器把新值写回 ``props.params`` 并发 ``point.updated``，于是概览页与
规则看到的是同一个值。插件不要在别处另存一份状态。

文件位置
--------

编译时查找出厂 JSON （产品目录有文件即停止，不再用仓库默认）：

1. ``application/APP/configs/`` 加文件名
2. 否则才用仓库 ``configs/`` 加同一文件名

设备首次启动后的活动副本：

* ``/data/config/adapters.json``
* ``/data/config/rules.json``
* ``/data/config/scenes.json``
* ``/data/config/devices.csv`` — 扁平化后的 **类型目录** （文案 / 控件）
* ``/data/config/model/`` — 可选上传的 ``devices.csv`` + ``parameters.csv``

编辑活动 JSON。``adapters.json`` 是 ``/data/config`` 下的运行时配置，不在在线更新范围内
（在线更新含固件、Web 控制台、LCD UI、设备资源）；它随出厂 / storage 镜像下发，或你直接用
REST 改（``PUT /api/config/adapters``）。规则与场景从各自页面导入，或用
``POST /api/rules/import`` / ``POST /api/scenes/import``。若接口未自动重载，再
``POST /api/hub/reload``。

类型目录 CSV 的远程更新与列说明见 :doc:`type_catalog`。

.. _adapters-JSON:

``adapters.json``
-----------------

根对象只有 ``adapters``。每个键是适配器名，须与固件里编进去的静态适配器、
或已安装商店模块的 ``adapter`` 字段一致。未识别的键会被忽略。有一个键被两处读取：
``mqtt_broker`` 既是 Hub 内置 broker 的监听配置，MQTT 客户端适配器也登记在这个
名字下，所以一个 ``enabled`` 同时决定 Hub 要不要监听、要不要连自己
（:doc:`networking`）。

顶层对象示例：

.. code-block:: JSON

   {
     "adapters": {
       "matter": {
         "enabled": true,
         "devices": []
       },
       "zigbee": { "enabled": false, "devices": [] },
       "ble_mesh": { "enabled": false, "provision": {}, "devices": [] },
       "thread": { "enabled": false, "dataset": null, "channel": null },
       "mqtt_broker": { "enabled": false, "port": 1883, "allow_anonymous": true, "users": [] },
       "modbus": { "enabled": false, "poll_ms": 2000, "gateways": [] }
     }
   }

通用字段：

* ``enabled`` — 只有当它是 JSON 布尔 ``false`` 时 Host 才跳过该适配器；缺省或
  写成非布尔值都会照常 ``init`` / ``start``。射频互斥仍可能单独拦住 ``start``
  （见 :doc:`radios` ）
* ``devices`` — 已绑定设备列表；字段因协议而异（见下）

各适配器 Host 会读的字段：

* **matter** — ``devices[]`` 的条数上限是 ``CONFIG_MINIHUB_MATTER_MAX_NODES``
  （缺省 128，可配 16–1024），超出的条目会丢弃并记一条错误日志。这里限制的是
  **配置清单** 的长度；运行中的控制器实际能跟踪并订阅多少节点还要更低，见
  :doc:`matter` 的限制一节。配网流程见 :doc:`matter`。字段：

  * ``point_id`` （必填）— 最长 64 字节
  * ``type`` — 最长 32 字节，缺省 ``light``
  * ``node_id`` — Fabric 节点号，十进制或 ``0x`` 十六进制字符串。控制器占用 1，设备应 ≥ 2
  * ``endpoint_id`` — Matter 端点，整数 0–65535，缺省 1（REST 配网写入的是这个字段，不是数组）
  * ``transport`` — ``wifi`` / ``ethernet`` / ``thread``，最长 15 字节；仅记录用
* **zigbee** — ``devices[]`` 最多 16 条，每条登记一个点位。字段：

  * ``point_id`` （必填）— Hub 点位名，最长 64 字节，两段点分形式，如
    ``zigbee.zb_0a1b`` （发现）或 ``zigbee.door_contact`` （手动绑定）
  * ``type`` （必填）— 点位类型，最长 32 字节。 ``light`` / ``switch`` 走开关命令
    （ ``turn_on`` / ``turn_off`` / ``toggle`` ）。 ``sensor`` 走温湿度或门磁
  * ``ieee`` — 64 位 IEEE 地址，十六进制字符串（可带 ``0x`` 前缀）。配网后用来认设备
  * ``short_addr`` — 网络短地址，整数 0–65535
  * ``endpoint`` — Zigbee 端点，整数 0–255，缺省 1
  * ``kind`` — 仅 ``type`` 为 ``sensor`` 时：设为 ``contact`` 则按 IAS Zone 门磁
    （点位值 ``open`` / ``closed`` ）
  * ``caps`` — 仅 ``type`` 为 ``sensor`` 且未设 ``kind`` 时：能力位图（整数）。
    未写 ``caps`` 时默认为温湿度（bit1 温度 | bit2 湿度）。其它位：bit0 开关、
    bit3 IAS Zone、bit4 电量测量、bit5 电能计量。非 ``sensor`` 一律按开关
    （bit0）处理，忽略 ``kind`` / ``caps``
* **ble_mesh** — 配网策略在 ``provision`` （也可把 ``uuid_filters`` / ``bind_models``
  写在适配器块顶层，方便 JSON inject）。 ``devices[]`` 最多 16 条。

  * ``provision.uuid_match_disabled`` — true 时接受任意未配网 UUID
  * ``provision.uuid_filters[]`` — 最多 8 条。 ``prefix_hex`` （或 ``hex`` ）为
    UUID 前缀十六进制，最多 16 字节； ``offset`` 为起始字节 0–15，且
    offset+长度 ≤ 16。无有效过滤器时等同接受全部 UUID
  * ``provision.bind_models[]`` — 最多 12 条。 ``model_id`` 为整数或
    ``0x1000`` 形式字符串；可选 ``name``、 ``company_id`` （SIG 默认
    ``0xFFFF`` ）。Host 会绑定能力表里已知的全部模型
    （OnOff ``0x1000``、Level ``0x1002``、Lightness ``0x1300``、
    CTL ``0x1303``、HSL ``0x1307``、Sensor Server ``0x1100``），
    **再加上** 此处列出的模型 —— 所以这个数组是追加厂商模型，
    不是替换内置集合
  * ``devices[]``： ``point_id``、 ``type`` （必填，最长 64 / 32 字节）。
    ``uuid`` 为 32 个十六进制字符
    （缓冲 32+NUL）。 ``unicast`` 0–65535。 ``elem_num`` 元素个数 0–255，
    缺省 1
  * ``devices[].models[]`` — 可选。该条目的 Composition 模型：整数、
    ``0x1000`` 形式字符串，或能力表公开的模型名字（``onoff``、
    ``gen_onoff_srv`` 等）。写了就由映射表按模型推能力、进而定类型；
    没写就由 ``type`` 推 —— 看映射表为该类型认识哪些参数。最终没有任何
    已知能力的条目会被跳过并记日志
* **thread** — 无 ``devices``， **不** 登记设备点（Border Router / dataset
  而已；Matter-over-Thread 终端走 ``adapters.matter``）。字段：

  * ``dataset`` — 运营数据集 TLV 的十六进制字符串（偶数位）。空或 ``null`` 时
    Host 会新建网络并回写。也可 ``GET`` / ``POST /api/thread/dataset``。
    二进制长度受 OpenThread 缓冲区限制（约 254 字节）
  * ``channel`` — 出厂种子带了这个键，但没有任何代码读写它：Host 忽略它，当前
    信道只由 ``GET /api/thread/status`` 报出。见 :doc:`thread`
* **mqtt_broker** — Hub 内置 broker 与它自己的环回客户端：没有 ``devices`` ，
  也不产生点位。字段： ``enabled`` （缺省 ``false`` ）、 ``port`` （1–65535，缺省 1883）、
  ``allow_anonymous`` （缺省 ``true`` ）、 ``users[]`` （最多 4 组
  ``name`` / ``pass`` ，各 31 字节）、 ``connect_rate_limit`` （每 10 秒窗口接受的
  CONNECT 数，缺省 16）。 ``enabled`` 为 ``true`` 时，Hub 还会连自己 ——
  ``127.0.0.1`` 上的 ``port`` ，客户端 ID 为 ``minihub_host`` —— JS 插件用的
  ``host.mqtt`` 就挂在这个客户端上。 ``allow_anonymous`` 设为 ``false`` 时，
  它用 ``users[0]`` 登录； ``users[]`` 为空就意味着 ``host.mqtt`` 连不上，
  并会记一条告警。监听地址、reload 行为与安全姿态见 :doc:`networking`
* **modbus** — 主站轮询。最多 8 个网关、全局最多 64 条映射。

  * ``poll_ms`` — 轮询周期，整数 ≥ 500，缺省 2000
  * ``gateways[]``：

    * ``id`` — 最长 31 字节，缺省 ``gw0``、 ``gw1`` …
    * ``transport`` — ``tcp`` （默认）/ ``rtu`` （可用 ``rs485`` / ``serial`` ）/
      ``rtuovertcp`` （可用 ``gateway`` ）
    * ``host`` — TCP / rtuovertcp 必填，最长 63 字节
    * ``port`` — TCP 端口 1–65535，缺省 502
    * ``timeout_ms`` — 缺省 1000
    * ``unit_id`` — 默认从站地址 0–255，缺省 1
    * RTU 另有： ``uart_port``、 ``baudrate`` （缺省 9600）、 ``parity``
      （ ``none`` / ``even`` / ``odd`` ）、 ``data_bits`` 5–8（缺省 8）、
      ``stop_bits`` 1 或 2、 ``tx_pin`` / ``rx_pin`` / ``rts_pin`` （缺省 -1）
  * ``maps[]`` （写在各网关下）把寄存器映到点位：

    * ``point_id`` （必填）— 最长 64 字节
    * ``type`` — 最长 32 字节，缺省 ``sensor``
    * ``unit_id`` — 覆盖网关默认从站号
    * ``fc`` — ``coil`` / ``discrete`` （或 ``di`` ）/ ``input`` （或 ``ir`` ）/
      其它视为 holding
    * ``address`` — PDU 地址，从 0 计，0–65535
    * ``count`` — 数量，缺省 1
    * ``datatype`` — ``u16`` （默认）/ ``s16`` / ``u32`` / ``s32`` / ``f32``
    * ``scale`` / ``offset`` — 读值换算，缺省 1.0 / 0.0
    * ``writable`` — true 时允许写
    * ``batch_as_props`` — ``count`` > 1 时，另把原始寄存器批次写进点位
      ``props``，键为 ``r0`` … ``r31`` （最多 32 个）

商店模块 (JS) 使用自己的键，例如 ``my_module``。安装时若槽位不存在会创建。
JSON 包把 ``inject`` **合并** 进指定适配器块（见 :doc:`plugins/develop_json` ）。

.. only:: not hide_l3

   ELF 模块同样使用自己的适配器键。

**不要** 把厂商云凭证写进通用 Host 的出厂配置。用户登录后由模块写入 NVS 或模块私有文件。

规则 (``rules.json``)
---------------------

``when`` / ``if`` / ``then`` 全表、比较 ``op`` （含 ``ne``）以及动作类型
（``command``、``command_by_type``、``notify``，另有遗留的 ``delay``）：
:doc:`rules`。导入：``POST /api/rules/import``。MCP：``rules.list`` /
``rules.put``。

规则读参数，不读界面。``when.parameter`` 写参数名（如 ``brightness``），比较的是
``props.params`` 里该项的 ``value``；不写 ``parameter`` 时才比较点位摘要 ``value``。
两种写法都在值 **变化进入** 命中状态时触发，值一直停在那里不会重复触发。
``then`` 用 ``command`` 动作改状态，所以规则同样要经过适配器，不能直接改
``props.params``。

场景 (``scenes.json``)
----------------------

场景是一组 **步骤**，步骤内可含多条指令、可重复，用于一键跨设备编排。顶层是
``scenes`` 数组，每项：

.. code-block:: JSON

   {
     "scenes": [
       {
         "id": "good_night",
         "name": "晚安",
         "steps": [
           {
             "actions": [
               { "point_id": "sim.js_kitchen", "cmd": "turn_off" },
               { "point_id": "sim.js_desk", "cmd": "set_brightness",
                 "params": { "parameter": "brightness", "value": 40 } }
             ],
             "loop": 1,
             "delay_after_ms": 0
           },
           {
             "actions": [ { "point_id": "sim.plug", "cmd": "turn_off" } ],
             "loop": 3,
             "delay_after_ms": 1000
           }
         ],
         "max_duration_ms": 0
       }
     ]
   }

字段：

* ``id`` — 场景标识（字符串），invoke 时按精确匹配。Web 编辑器用「名称 + 时间戳」
  生成它，编辑表单里不显示；但 ``name`` 为空时界面会退回显示 ``id``，删除确认框里
  也会带上它。没有 ``id`` 的场景能被加载和列出，却永远无法被 invoke；``id`` 重复时
  只有第一条可用。
* ``name`` — 展示名。只有 Web 编辑器要求必填，加载器接受没有 ``name`` 的场景。
* ``steps[]`` — 步骤序列，**依次** 串行执行：

  * ``actions[]`` — 本步要下发的指令；步内动作按数组顺序下发
    （``cmd`` 走 ``minihub_command_invoke``，与 ``POST /api/command`` 同一路径）。
    每条 ``action`` 的 ``point_id`` 与 ``cmd`` 必须是字符串，否则该动作被跳过；
    ``params`` 可选，且只有 JSON 对象才会被保留。
  * ``loop`` — 本步重复次数。小于 1、非数字都按 1 处理；**没有上限**，
    失控的重复只能靠 ``max_duration_ms`` 拦。
  * ``delay_after_ms`` — **每一轮** 结束后都要等待的毫秒数，缺省 0。所以
    ``loop: 3`` 配 ``delay_after_ms: 1000`` 一共睡 3 秒，最后一轮的动作之后也照睡。
    负数或没有都按 0。延时按 200 ms 分片执行，以便取消被尽快发现。

* ``max_duration_ms`` — 安全上限（毫秒），只有正数才被保留，``0`` 或不写表示不限。
  截止点在每条动作前、每个 200 ms 延时片前检查，一旦越界整场运行立即中止（不是等
  这一步跑完）；它打断不了已经在执行的适配器命令。Web 端会算出「预计总用时」
  （``Σ(loop × delay_after_ms)``）并拒绝填一个 **大于** 它的上限。

``params`` 的形状由命令与适配器决定。概览页控件统一发 ``parameter`` 与 ``value``
两个字段；BLE Mesh 的 ``set_brightness`` / ``set_color_temp`` 直接读 ``brightness`` /
``cct``，Modbus 的 ``set`` 只读 ``value``。命令生效后由适配器回写 ``props.params``，
场景本身不写点位。

加载器会静默套用的上限：**8** 个场景（``MAX_SCENES``）、单场景 **5** 步
（``MAX_SCENE_STEPS``）、单步 **8** 条动作（``MAX_STEP_ACTIONS``），超出的条目被丢弃；
字符串会被截断：``id`` 31 字符、``name`` 63、``point_id`` 63、``cmd`` 31。Web
编辑器把单步 ``delay_after_ms`` 限制在 **7200 秒（7,200,000 毫秒）** 以内，加载器
不管 —— 手写进 ``scenes.json`` 的更长值会被保留，拦住它的是 ``max_duration_ms``。

**向后兼容**：没有 ``steps`` 时，旧版 ``commands:[{point_id,cmd,delay_ms}]`` 平铺
格式仍可被加载，每条命令变成一个只含单条动作、``loop=1``、
``delay_after_ms=delay_ms`` 的步骤。但这条旧路径只认这三个键：旧格式里的
``params`` 会被固件丢掉（Web 编辑器会带上它，并在保存时改写成 ``steps``）。

执行：``POST /api/scenes/invoke``，``{"scene_id":"good_night"}`` （也接受键名 ``id``；
请求体上限 128 字节）。HTTP 200 只说明运行已 **排队** 到场景 worker，
不代表动作执行完毕。新的 invoke、配置重载或场景导入会请求正在运行的场景停止，
而它只在动作之间或延时分片之间才发现 —— 取消不是即时的，下一个场景也要等当前这个
返回后才开始。场景只按 ``id`` 匹配，不按 ``name``，事件总线上也没有场景进度可订阅。

事件
----

适配器与 JS 模块往 Hub 事件总线发布。Web UI 订阅 ``GET /ws/events``。
规则监听 ``point.updated``。事件类型见 :doc:`api/host_rest` （``GET /ws/events``）。
插件不必注册 WebSocket — 更新点位/发命令即可。

相关
----

* 类型目录与 UI：:doc:`type_catalog`
* 规则引擎：:doc:`rules`
* REST 示例：:doc:`api/host_rest`
* 端到端流程：:doc:`data_flow`
* Matter 配网 REST：:doc:`matter`
* JSON inject 合并：:doc:`plugins/develop_json`

.. only:: not hide_l3

   * ELF 插件：:doc:`plugins/develop_elf`

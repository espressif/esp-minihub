数据模型
========

:link_to_translation:`en:[English]`

本章描述 **运行时点位模型** 与 Hub 配置 （``adapters.json``、场景）。
规则见 :doc:`rules`。设备页用的类型、控件与图标见 :doc:`type_catalog`。

当前模型长什么样
----------------

MiniHub Core **按点位工作**，不按灯、开关等 C 结构体工作：

* **一台物理设备对应一条** ``minihub_point_t`` （Matter 按 endpoint，
  见 :doc:`matter`）
* ``point_id`` = ``协议.设备名`` （如 ``sim.color_bulb``、
  ``zigbee.zb_7d4b``、``thread.matter_n2_ep1``）
* 实时参数都在该点的 ``props.params.<参数名>`` 里
* ``type`` 是字符串 （``light`` / ``switch`` / …），**不是** Core 里的封闭枚举
* **怎么画 UI** 来自 Host **类型目录** （CSV），与协议无关 —— 见
  :doc:`type_catalog`

.. code-block:: text

   物理设备
      │  适配器 / 插件登记
      ▼
   point_id = protocol.device     type = "light" | "switch" | …
      │
      ├─ value          可选摘要（常用电源 on/off）
      ├─ online
      └─ props.params
            ├─ power       { value, access, … }
            ├─ brightness  { value, access, min, max, … }
            └─ …

   Host 类型目录（CSV）              插件实例（devices.json）
   类型有哪些参数、标签、控件、默认范围     这台设备用哪些参数、初值、是否隐藏
          \                              /
           UI = 目录 ∩ 在线 props.params

**不要** 按参数再拆点 （错误示例：``sim.color_bulb.brightness``）。

活动点示例 （``GET /api/points``）：

.. code-block:: json

   {
     "point_id": "sim.color_bulb",
     "type": "light",
     "adapter": "color_bulb",
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
``owner``         厂商插件认领后，若 ``adapters.json`` 里该插件槽为
                  ``enabled``，总览把卡片从射频分组挪到该适配器分组。
                  **不要** 改 ``point.adapter`` （命令仍走 Zigbee 等 Host）。
``ota``           对象：``available`` （布尔），可选 ``version`` /
                  ``url`` / ``file`` / ``status`` 。**仅 Store 插件** 写入。
                  ``POST /api/devices/ota`` 优先把 ``props.ota.file`` 当
                  ``path``，否则用 ``url``。Host 内置适配器不得伪造可升级状态。
================  ============================================================

内置适配器只填 **协议能确定** 的字段（如 Zigbee Basic 的 ManufacturerName /
ModelIdentifier / ApplicationVersion / HWVersion；BLE Mesh Composition 的
CID/PID/VID 与 ``uuid``；Matter Basic Information 与 ``node_id``）。品类名、OTA、``owner``
由插件通过 ``minihub_point_register`` / ``minihub_point_merge_props`` 提供。
演示 ``color_bulb`` / ``meter_plug`` / ``color_bulb_js`` 在登记时写入
``manufacturer=Espressif`` 与产品 ``category``。

带新 ``props`` 的 ``minihub_point_set`` 会发 ``point.updated``
（WebSocket ``GET /ws/events``）。容量上限见 :doc:`api/minihub_core`。

发布实时参数的适配器应调用 ``minihub_point_merge_params`` （或
``minihub_point_merge_props``），以免冲掉同一点上的其他顶层键
（``ota``、``ieee``、``uuid``、``node_id``、``manufacturer``、``category``、``model``、
``sw_version``、``hw_version``、``owner`` 等）。插件在 **同一** ``point_id`` 上补充字段，不得为同一
物理设备再登记第二个点。

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

用 Web UI 或 REST 编辑活动 JSON
(``PUT /api/config/adapters``、``POST /api/rules/import``、
``POST /api/scenes/import``)；若接口未自动重载，再
``POST /api/hub/reload``。

类型目录 CSV 的远程更新与列说明见 :doc:`type_catalog`。

.. _adapters-json:

``adapters.json``
-----------------

根对象只有 ``adapters`` 。每个键是适配器名，须与固件里编进去的静态适配器、
或已安装 Store 模块的 ``adapter`` 字段一致。未识别的键会被忽略。

顶层对象示例：

.. code-block:: json

   {
     "adapters": {
       "matter": {
         "enabled": true,
         "devices": []
       },
       "zigbee": { "enabled": false, "devices": [] },
       "ble_mesh": { "enabled": false, "provision": {}, "devices": [] },
       "thread": { "enabled": false, "dataset": null, "channel": null },
       "mqtt": { "enabled": false, "broker": "127.0.0.1", "port": 1883, "devices": [] },
       "modbus": { "enabled": false, "poll_ms": 2000, "gateways": [] }
     }
   }

通用字段：

* ``enabled`` — 为 true 时 Host 才对该适配器 ``init`` / ``start``
* ``devices`` — 已绑定设备列表；字段因协议而异（见下）

各适配器 Host 会读的字段：

* **matter** — ``devices[]`` 最多 16 条，每条一个点位。配网流程见 :doc:`matter` 。字段：

  * ``point_id`` （必填）— 最长 64 字节
  * ``type`` — 最长 16 字节，缺省 ``light``
  * ``node_id`` — Fabric 节点号，十进制或 ``0x`` 十六进制字符串。控制器占用 1，设备应 ≥ 2
  * ``endpoint_id`` — Matter 端点，整数 0–65535，缺省 1（REST 配网写入的是这个字段，不是数组）
  * ``transport`` — ``wifi`` / ``ethernet`` / ``thread`` ，最长 15 字节；仅记录用
* **zigbee** — ``devices[]`` 最多 16 条，每条登记一个点位。字段：

  * ``point_id`` （必填）— Hub 点位名，最长 64 字节，两段点分形式，如
    ``zigbee.zb_0a1b`` （发现）或 ``zigbee.door_contact`` （手动绑定）
  * ``type`` （必填）— 点位类型，最长 16 字节。 ``light`` / ``switch`` 走开关命令
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
  写在适配器块顶层，方便 L1 inject）。 ``devices[]`` 最多 16 条。

  * ``provision.uuid_match_disabled`` — true 时接受任意未配网 UUID
  * ``provision.uuid_filters[]`` — 最多 8 条。 ``prefix_hex`` （或 ``hex`` ）为
    UUID 前缀十六进制，最多 16 字节； ``offset`` 为起始字节 0–15，且
    offset+长度 ≤ 16。无有效过滤器时等同接受全部 UUID
  * ``provision.bind_models[]`` — 最多 12 条。 ``model_id`` 为整数或
    ``0x1000`` 形式字符串；可选 ``name`` 、 ``company_id`` （SIG 默认
    ``0xFFFF`` ）。Host 客户端目前覆盖 OnOff ``0x1000`` 、Level ``0x1002`` 、
    Lightness ``0x1300`` 、CTL ``0x1303``
  * ``devices[]`` ： ``point_id`` 、 ``type`` （必填，最长 64 / 16 字节）。
    ``type`` 为 ``sensor`` 的条目会被跳过。 ``uuid`` 为 32 个十六进制字符
    （缓冲 32+NUL）。 ``unicast`` 0–65535。 ``elem_num`` 元素个数 0–255，
    缺省 1
* **thread** — 无 ``devices`` ， **不** 登记设备点（Border Router / dataset
  而已；Matter-over-Thread 终端走 ``adapters.matter``）。字段：

  * ``dataset`` — 运营数据集 TLV 的十六进制字符串（偶数位）。空或 ``null`` 时
    Host 会新建网络并回写。也可 ``GET`` / ``POST /api/thread/dataset`` 。
    二进制长度受 OpenThread 缓冲区限制（约 254 字节）
  * ``channel`` — 状态回写（当前信道），启动不依赖此项。见 :doc:`thread`
* **mqtt** — 连接 Broker 并登记点位。 ``devices[]`` 最多 16 条。

  * ``broker`` — 主机名或 IP，最长 63 字节。未配置则点位仍注册，但保持离线
  * ``port`` — Broker 端口，缺省 1883
  * ``devices[]`` ： ``point_id`` 、 ``type`` （必填）。 ``state_topic`` /
    ``command_topic`` 最长 95 字节。命令 ``turn_on`` / ``turn_off`` /
    ``toggle`` 向 ``command_topic`` 发布 ``ON`` / ``OFF`` ；订阅
    ``state_topic`` 把载荷映成点位 ``on`` / ``off``
* **modbus** — 主站轮询。最多 8 个网关、全局最多 64 条映射。

  * ``poll_ms`` — 轮询周期，整数 ≥ 500，缺省 2000
  * ``gateways[]`` ：

    * ``id`` — 最长 31 字节，缺省 ``gw0`` 、 ``gw1`` …
    * ``transport`` — ``tcp`` （默认）/ ``rtu`` （可用 ``rs485`` / ``serial`` ）/
      ``rtuovertcp`` （可用 ``gateway`` ）
    * ``host`` — TCP / rtuovertcp 必填，最长 63 字节
    * ``port`` — TCP 端口 1–65535，缺省 502
    * ``timeout_ms`` — 缺省 1000
    * ``unit_id`` — 默认从站地址 0–255，缺省 1
    * RTU 另有： ``uart_port`` 、 ``baudrate`` （缺省 9600）、 ``parity``
      （ ``none`` / ``even`` / ``odd`` ）、 ``data_bits`` 5–8（缺省 8）、
      ``stop_bits`` 1 或 2、 ``tx_pin`` / ``rx_pin`` / ``rts_pin`` （缺省 -1）
  * ``maps[]`` （写在各网关下）把寄存器映到点位：

    * ``point_id`` （必填）— 最长 64 字节
    * ``type`` — 最长 16 字节，缺省 ``sensor``
    * ``unit_id`` — 覆盖网关默认从站号
    * ``fc`` — ``coil`` / ``discrete`` （或 ``di`` ）/ ``input`` （或 ``ir`` ）/
      其它视为 holding
    * ``address`` — PDU 地址，从 0 计，0–65535
    * ``count`` — 数量，缺省 1
    * ``datatype`` — ``u16`` （默认）/ ``s16`` / ``u32`` / ``s32`` / ``f32``
    * ``scale`` / ``offset`` — 读值换算，缺省 1.0 / 0.0
    * ``writable`` — true 时允许写
    * ``batch_as_props`` — true 时一批寄存器写入点位 ``props``

Store 模块 (L2/L3) 使用自己的键，例如 ``my_module`` 。安装时若槽位不存在会创建。
L1 包把 ``inject`` **合并** 进指定适配器块（见 :doc:`plugins/develop_l1` ）。

**不要** 把厂商云凭证写进通用 Host 的出厂配置。用户登录后由模块写入 NVS 或模块私有文件。

规则 (``rules.json``)
---------------------

``when`` / ``if`` / ``then`` 全表、比较 ``op`` （含 ``ne``）以及四种动作：
:doc:`rules`。导入：``POST /api/rules/import``。MCP：``rules.list`` /
``rules.put``。

场景 (``scenes.json``)
----------------------

.. code-block:: json

   {
     "scenes": [
       {
         "id": "all_off",
         "name": "全关",
         "commands": [
           { "point_id": "sim.color_bulb", "cmd": "turn_off", "delay_ms": 0 }
         ]
       }
     ]
   }

执行：``POST /api/scenes/invoke``，``{"scene_id":"all_off"}``。

事件
----

适配器与 JS/原生模块往 Hub 事件总线发布。Web UI 订阅 ``GET /ws/events``。
规则监听 ``point.updated``。事件类型见 :doc:`api/host_rest` （``GET /ws/events``）。
插件不必注册 WebSocket — 更新点位/发命令即可。

相关
----

* 类型目录与 UI：:doc:`type_catalog`
* 规则引擎：:doc:`rules`
* REST 示例：:doc:`api/host_rest`
* 端到端流程：:doc:`architecture_overview`
* Matter 配网 REST：:doc:`matter`
* L1 inject 合并：:doc:`plugins/develop_l1`
* L3 插件：:doc:`plugins/develop_l3`

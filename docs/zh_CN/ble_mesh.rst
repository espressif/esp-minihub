BLE Mesh
========

:link_to_translation:`en:[English]`

MiniHub 以 **Host 适配器** 运行 BLE Mesh。设备条目在 ``adapters.ble_mesh`` 下。
JSON 字段（含 ``provision.uuid_filters`` 与 ``provision.bind_models`` ）见
:doc:`data_model` （ :ref:`adapters-JSON` ）。

在 ``sdkconfig`` / menuconfig 中打开 ``CONFIG_MINIHUB_BLE_MESH_SDK`` 后直接编译
（默认协议栈已含 BLE Mesh；若该选项已关掉，脚本会丢掉对应栈）：

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build

下列 REST 仅在使能 ``CONFIG_MINIHUB_BLE_MESH_SDK`` 时编入固件。启动与 IP 上行见
:doc:`getting_started`、 :doc:`networking`。

共存
----

Zigbee 入网窗口与 BLE Mesh 配网窗口不能同时打开。固件会返回错误字符串（例如
Zigbee 正在入网，或 BLE Mesh 已在配网）。可用 ``GET /api/radio/provision``
查询射频 / 配网状态。

运行时配置
----------

在 ``/data/config/adapters.json`` 中启用：

.. code-block:: JSON

   {
     "adapters": {
       "ble_mesh": {
         "enabled": true,
         "provision": {},
         "devices": []
       }
     }
   }

设置 ``adapters.ble_mesh.enabled``。配网策略在 ``provision`` （
``uuid_filters`` / ``bind_models`` ）。已绑定节点在 ``devices[]``。
条目带 ``models[]`` 时由它决定能力；不带时由 ``type`` 决定 —— 看能力映射表为
该类型认识哪些参数。最终没有任何已知能力的条目会被跳过并记日志。
字段见 :ref:`adapters-JSON`。
JSON 包可 **合并** 额外 bind/过滤默认值（演示
``examples/plugins/ble_mesh_pack`` ）； **不要** 用 JSON 发明 ``devices[]``。

能力映射表
----------

适配器对节点做的每件事都来自一张表：固件镜像里的
``components/minihub_ble_mesh/config/ble_mesh_map.json``。它描述
*model → 能力 → 类型 → 参数 → 命令*：

* ``models[]`` — Hub 认识的 Composition 模型，各自带能力位、对应的客户端模型、
  读回状态用的 ``get`` opcode，以及 ``devices[]`` 条目可以写的名字
  （``cap`` 与公开的 ``alias`` ）
* ``classify[]`` — 哪些能力、哪些传感器属性让一个节点成为 ``light`` /
  ``switch`` 或某个传感器类型
* ``params[]`` — 节点暴露的参数，含访问权限与默认值
* ``controls[]`` — 哪个参数由哪条 Mesh 消息驱动
* ``sensors[]`` — 传感器属性如何解码：字节数、有无符号、缩放、「值未知」
  哨兵与目录上下界

这张表决定绑定名单、AppKey 绑定的客户端模型、种子参数与下发命令。新增一种设备
类型是改 JSON，不是改固件。``provision.bind_models`` 里的策略 **不会** 替换这份
绑定名单：Host 绑定表里已知的全部模型，**再加上** 策略点名的模型 —— 所以 JSON
包可以追加厂商模型，而不会挤掉内置那些。

OEM 不重编也能覆盖这张表：把同样结构的文件放在
``/store/preinstall/ble_mesh_map.json`` （modstore 镜像）或
``/data/config/ble_mesh_map.json`` （storage 分区）。靠后的文件替换它点名的行，
没点名的行保留内置值。

``tools/test_ble_mesh_map.py`` 在这张表与组件目录（widget catalog）、适配器查找
的控制字段、仓库公开的模型名字，或它引用的蓝牙 SIG 特性尺寸发生漂移时报错。

Composition 没有命中任何 ``classify[]`` 规则的节点 **不会** 获得点位：它只被
记日志并计入未分类计数，绝不冒充成可控开关。

设备类型
--------

``light``
   OnOff / Level / Lightness / CTL / HSL。

``switch``
   仅 OnOff。

BLE Mesh 上做不到的：

* ``intensity`` —— Mesh 灯光模型没有强度通路。类型目录给 ``light`` 列了它，
  但 Mesh 适配器从不种子它，写入它返回 ``ESP_ERR_NOT_SUPPORTED``。
* ``sensor_contact`` / ``sensor_smoke`` / ``sensor_tamper`` / ``sensor_water`` /
  ``pm25`` / ``speaker`` —— 蓝牙 SIG 没有对应的传感器属性，也没有音频模型。
  只能靠厂商模型承载，映射表变不出来。
* ``battery`` —— Generic Battery 模型尚未进表。

状态同步
--------

出厂的 ``ble_mesh_map.json`` 带有 ``report`` 块。绑定完成后，每个本地 client model
会订阅该 group，节点上每个被绑定的 Server model 都会收到一条 Config Model
Publication Set。于是发布的 Status 与 Get 的回复走同一套解码：generic 与 light
回调以 publish 事件为门（ ``ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT`` 、
``ESP_BLE_MESH_LIGHT_CLIENT_PUBLISH_EVT`` ），sensor 回调不看事件、只按 opcode
分派。墙面开关或插座发布 OnOff Status 时，Hub 点位不必再发一次 Get 就会更新。

Get 路径（绑定完成、Set 回复、Hub 重启时的读回；无 ``report`` 时状态同步也只靠它）：

* model 绑定完成时：每个已绑定且 ``models[]`` 行声明了 ``get`` opcode 的模型各取
  一次，带 Sensor Server 的节点还要取 Sensor Descriptor。
* descriptor 回答之后再取一次：它给出 property id，一条不带 property 操作数的
  Sensor Get 就把全部属性取回。
* 每次 Set 的回复。
* Hub 重启时：按 NVS 里随设备保存的 ``(model, elem)`` 绑定列表再 Get 一遍（已存过的
  Sensor Server 只发 Sensor Get，不再 Descriptor）。

``ble_mesh_adapter_poll()`` 直接返回：适配器轮询任务不会去取 Mesh 状态。映射表
标为可读的参数背后都有一条 Get。在 Get 或发布的 Status 到来之前，参数显示的是映射表
里的 ``init`` 值。在绑定列表出现之前配过网的设备仍能控制，但会停在 ``init``，直到有
命令或重新配网。``report`` 首次启用时已经在网上的节点，需要重新配网（或补一条
Publication Set）才会向 Hub 的 group 发布。

默认 ``report`` 块：

.. code-block:: json

   "report": { "group": "0xC001", "period_s": 30, "retransmit_count": 2,
               "retransmit_ms": 200, "ttl": 3 }

去掉该块，或用一份不含它的 overlay：Hub 不向节点写 Publication，本地 client
model 也不订阅状态 group；状态同步只靠上面的 Get 路径。

字段取值：

* ``period_s`` —— 必须能被 Model Publication Period 那个字节精确编码。63 秒以内按
  1 秒步进，之后按 10 秒、再按 10 分钟步进。编码不出的值只记日志并拒绝，不会就近
  取整。
* ``group`` —— 必须是 group 地址。``ESP_BLE_MESH_ADDR_IS_GROUP`` 拒绝的值会让上报
  保持关闭，只留一行日志。
* ``ttl`` —— 取 0–127；255 表示沿用节点自己的默认值。

节点拒绝该周期时，Hub 会不带周期再配一次，该节点此后只在状态变化时发布。

REST 接口
---------

总览见 :doc:`api/host_rest`。

* ``POST /api/ble_mesh/permit_provision`` — 打开或关闭配网窗口
* ``POST /api/ble_mesh/devices`` — 删除设备（遗留接口；适配器 ``ble_mesh`` ）
* ``GET /api/radio/provision`` — 射频 / 配网共存状态。这一条 **不** 受
  ``CONFIG_MINIHUB_BLE_MESH_SDK`` 门控：它始终注册，没编进去的栈就回 ``false``

配网窗口
^^^^^^^^

请求体 JSON 可选。 ``seconds`` 缺省 60。 ``0`` 关闭窗口。
LCD 概览在 BLE Mesh 适配器启用时也有「允许入网」按钮，与 Web 调用同一
``permit_provision`` 路径；开窗 / 关窗会发 ``radio.permit_opened`` /
``radio.permit_closed``，屏与 Web 都会刷新状态。

窗口实际长度并不等于 ``seconds``：到期时若仍有配网进行中，Hub 会把它 **延长一次**
30 秒，第二次到期就中止卡住的配网并关窗。所以 60 秒的请求最长会占用射频约 90 秒。
另请注意 BLE Mesh 属于 Wi-Fi 那一组： ``adapters.ble_mesh.enabled`` 与
``adapters.zigbee.enabled`` 同时为真时，留下的是 Wi-Fi 与 BLE Mesh，运行时被丢掉
的是 802.15.4 那个适配器，只留一行
``adapter 'zigbee' not started (radio mutex)`` 日志（见 :doc:`radios`）。

.. code-block:: bash

   curl -s -X POST http://HUB/api/ble_mesh/permit_provision \
     -H 'Content-Type: application/json' \
     -d '{"seconds":60}'

移除设备（遗留）
^^^^^^^^^^^^^^^^

``POST /api/ble_mesh/devices`` 用于移除设备。若请求体未写 ``adapter``，
处理函数会注入 ``ble_mesh``。语义与 Hub 点位移除相同。

.. code-block:: JSON

   { "point_id": "mesh.light.1" }

射频 / 配网状态
^^^^^^^^^^^^^^^^

``GET /api/radio/provision`` 返回 JSON，包括 Zigbee / BLE Mesh / Matter 是否编入、
Zigbee 入网或 BLE Mesh 配网是否进行中，以及表示射频关键窗口的 ``busy`` 标志。
再开另一个入网/配网窗口前先查此接口。

控制
----

节点绑定后，用 ``POST /api/command`` 与 Hub ``point_id`` 控制（OnOff 模型：
``turn_on`` / ``turn_off`` / ``toggle`` ）。见 :doc:`api/host_rest`。

点位身份与插件扩展
------------------

每台 BLE Mesh 节点对应 **一条** Host 点（``ble_mesh.bm_XXXX``，与 Zigbee 相同的
``<protocol>.<device>`` 约定）。旧版 ``{type}/bm_XXXX`` 在 NVS 加载时迁移。登记时
适配器写入顶层身份字段（不是类型目录参数 / 概览页滑条）：

* ``props.uuid`` — 节点 Device UUID 十六进制（Mesh 没有 Zigbee ``ieee``）
* ``props.manufacturer`` — Composition Page 0 **Company ID** （Bluetooth CID）。
  ``0x02E5`` 写成 ``Espressif``，其余写成 ``0xNNNN``。**不是** ZCL Basic 字符串
* ``props.model`` / ``props.sw_version`` — Product ID / Version ID 的 ``0xNNNN``
* ``props.category`` — BLE Mesh Host **不** 填写。缺省时 UI 用类型目录标签
  （``light`` →「灯」）。插件可 merge 产品名作为品类

运行参数（``props.params.<name>.value``）登记时按节点能力种子 —— 已配网节点看
Composition，``devices[]`` 条目看声明的 ``type`` —— 默认值取 ``params[]`` 里写的
（``power`` / ``brightness`` / ``cct`` / ``hue`` / ``saturation``），遥测与 ``set``
命令按读-改-写更新（``brightness`` → Lightness/Level，``hue``/``saturation`` → HSL Set，
``cct`` → CTL）。**不得** ``minihub_point_set`` 整份替换 ``props``，否则会冲掉
uuid / 厂商 / ``owner`` / ``ota`` / 其他参数。

插件（JS）按 ``props.manufacturer`` 匹配（本地过滤，如打包的
``catalog.json``），写在同一 ``point_id`` 上：

* **关联** — 写入 ``props.owner`` = 插件 ``adapter`` 名。该槽在
  ``adapters.json`` 为 ``enabled`` 时，总览把卡片挪到厂商分组（文案见
  ``meta.json`` ``filter_label``）。**不需要** 云端目录。
* **OTA 标记** — 由插件在自己的检查判定有固件可升时 merge ``props.ota``
  （``available``、``version``，可选 ``url`` / ``file`` / ``size``）；其中 Hub
  只会用 ``file`` / ``url`` / ``version``。示例::

     {
       "ota": {
         "available": true,
         "version": "1.2.3",
         "url": "https://vendor.example/fw.bin",
         "file": "/data/ota/aabbccddeeff0011.bin",
         "size": 524288
       }
     }

启动终端 OTA（Mesh Firmware Update / BLOB）可用适配器命令 ``ota`` /
``start_ota`` （``params`` 中可选 ``path`` / ``file_path`` / ``url``），或 Host
``minihub_ble_mesh_ota_offer(point_id, file_path)``。空中传图未接通前，这些
接口返回 ``ESP_ERR_NOT_SUPPORTED`` 并打清晰日志 — 插件现在仍可发布
``props.ota``。

``MINIHUB_PLUGIN_DIRS`` 含该插件目录时才会打包 ``.mhub``。

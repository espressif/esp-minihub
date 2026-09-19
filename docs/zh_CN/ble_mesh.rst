BLE Mesh
========

:link_to_translation:`en:[English]`

MiniHub 以 **Host 适配器** 运行 BLE Mesh。设备条目在 ``adapters.ble_mesh`` 下。
JSON 字段（含 ``provision.uuid_filters`` 与 ``provision.bind_models`` ）见
:doc:`data_model` （ :ref:`adapters-json` ）。

在 ``sdkconfig`` / menuconfig 中打开 ``CONFIG_MINIHUB_BLE_MESH_SDK`` 后直接编译
（默认协议栈已含 BLE Mesh；若该选项已关掉，脚本会丢掉对应栈）：

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build

下列 REST 仅在使能 ``CONFIG_MINIHUB_BLE_MESH_SDK`` 时编入固件。启动与 IP 上行见
:doc:`getting_started` 、 :doc:`networking` 。

共存
----

Zigbee 入网窗口与 BLE Mesh 配网窗口不能同时打开。固件会返回错误字符串（例如
Zigbee 正在入网，或 BLE Mesh 已在配网）。可用 ``GET /api/radio/provision``
查询射频 / 配网状态。

运行时配置
----------

在 ``/data/config/adapters.json`` 中启用：

.. code-block:: json

   {
     "adapters": {
       "ble_mesh": {
         "enabled": true,
         "provision": {},
         "devices": []
       }
     }
   }

设置 ``adapters.ble_mesh.enabled`` 。配网策略在 ``provision`` （
``uuid_filters`` / ``bind_models`` ）。已绑定节点在 ``devices[]`` 。
``type`` 为 ``sensor`` 的条目会被跳过。字段见 :ref:`adapters-json` 。
L1 包可 **合并** 额外 bind/过滤默认值（演示
``examples/plugins/ble_mesh_pack`` ）； **不要** 用 L1 发明 ``devices[]`` 。

REST 接口
---------

总览见 :doc:`api/host_rest` 。

* ``POST /api/ble_mesh/permit_provision`` — 打开或关闭配网窗口
* ``POST /api/ble_mesh/devices`` — 删除设备（遗留接口；适配器 ``ble_mesh`` ）
* ``GET /api/radio/provision`` — 射频 / 配网共存状态

配网窗口
^^^^^^^^

请求体 JSON 可选。 ``seconds`` 缺省 60。 ``0`` 关闭窗口。

.. code-block:: bash

   curl -s -X POST http://HUB/api/ble_mesh/permit_provision \
     -H 'Content-Type: application/json' \
     -d '{"seconds":60}'

移除设备（遗留）
^^^^^^^^^^^^^^^^

``POST /api/ble_mesh/devices`` 用于移除设备。若请求体未写 ``adapter`` ，
处理函数会注入 ``ble_mesh`` 。语义与 Hub 点位移除相同。

.. code-block:: json

   { "point_id": "mesh.light.1" }

射频 / 配网状态
^^^^^^^^^^^^^^^^

``GET /api/radio/provision`` 返回 JSON，包括 Zigbee / BLE Mesh / Matter 是否编入、
Zigbee 入网或 BLE Mesh 配网是否进行中，以及表示射频关键窗口的 ``busy`` 标志。
再开另一个入网/配网窗口前先查此接口。

控制
----

节点绑定后，用 ``POST /api/command`` 与 Hub ``point_id`` 控制（OnOff 模型：
``turn_on`` / ``turn_off`` / ``toggle`` ）。见 :doc:`api/host_rest` 。

点位身份与厂商扩展
------------------

每台 BLE Mesh 节点对应 **一条** Host 点（``light/bm_XXXX`` 等现有格式）。登记时
适配器写入顶层身份字段（不是类型目录参数 / 设备页滑条）：

* ``props.uuid`` — 节点 Device UUID 十六进制（Mesh 没有 Zigbee ``ieee``）
* ``props.manufacturer`` — Composition Page 0 **Company ID**（Bluetooth CID）。
  ``0x02E5`` 写成 ``Espressif``，其余写成 ``0xNNNN``。**不是** ZCL Basic 字符串
* ``props.model`` / ``props.sw_version`` — Product ID / Version ID 的 ``0xNNNN``
* ``props.category`` — BLE Mesh Host **不** 填写。缺省时 UI 用类型目录标签
  （``light`` →「灯」）。厂商插件可 merge 产品名作为品类

遥测（含 CTL 色温 / 亮度）只通过 ``minihub_point_merge_params`` 或只改
``value``，**不得** ``minihub_point_set`` 整份替换 ``props``，否则会冲掉
uuid / 厂商 / ``owner`` / ``ota``。

厂商插件（L2/L3）按 ``props.manufacturer`` 匹配（本地过滤，如打包的
``catalog.json``），写在同一 ``point_id`` 上：

* **认领** — 写入 ``props.owner`` = 插件 ``adapter`` 名。该槽在
  ``adapters.json`` 为 ``enabled`` 时，总览把卡片挪到厂商分组（演示
  ``examples/plugins/ble_mesh_ota``，文案见 ``ui.json`` ``filter_label``）。
  **不需要** 云端目录。
* **OTA 标记** — 仅在 GET ``catalog_url`` 成功后 merge ``props.ota``
  （``available``、``version``，可选 ``url`` / ``file`` / ``size``）。示例::

     {
       "ota": {
         "available": true,
         "version": "1.2.3",
         "url": "https://vendor.example/fw.bin",
         "file": "/data/ble_mesh_ota/aabbccddeeff0011.bin",
         "size": 524288
       }
     }

启动终端 OTA（Mesh Firmware Update / BLOB）可用适配器命令 ``ota`` /
``start_ota`` （``params`` 中可选 ``path`` / ``file_path`` / ``url``），或 Host
``minihub_ble_mesh_ota_offer(point_id, file_path)``。空中传图未接通前，这些
接口返回 ``ESP_ERR_NOT_SUPPORTED`` 并打清晰日志 — 插件现在仍可发布
``props.ota``。

``MINIHUB_PLUGIN_DIRS`` 含 ``examples/plugins`` （或该插件目录）时才会打包
``.mhub``；不要把 ``ble_mesh_ota`` 写进公开 Host ``store_preinstall``。

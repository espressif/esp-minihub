Zigbee
======

:link_to_translation:`en:[English]`

MiniHub 以 **Host 适配器** （协调器）运行 Zigbee，而不是独立终端示例。设备条目在
``adapters.zigbee`` 下。JSON 字段见 :doc:`data_model` （ :ref:`adapters-json` ）；
本章覆盖构建、入网与 REST。

Zigbee 与 Thread 共用 802.15.4，**编译期互斥**。Hub 启动与 IP 上行见
:doc:`getting_started` 与 :doc:`networking`。

构建
----

使能 ``CONFIG_MINIHUB_ZIGBEE_SDK`` 后编译：

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build

下列 REST 随该选项编入固件。

运行时配置
----------

在 ``/data/config/adapters.json`` 中启用（出厂配置：
``application/minihub/configs/adapters.json`` ）：

.. code-block:: json

   {
     "adapters": {
       "zigbee": {
         "enabled": true,
         "devices": []
       }
     }
   }

设置 ``adapters.zigbee.enabled`` ，并在 ``devices[]`` 中列出已绑定设备。
字段清单见 :ref:`adapters-json` 。

REST 接口
---------

仅当编译了 ``CONFIG_MINIHUB_ZIGBEE_SDK`` 时存在。鉴权与总览见
:doc:`api/host_rest` 。

* ``POST /api/zigbee/permit_join`` — 打开或关闭入网窗口
* ``POST /api/zigbee/install_code`` — 入网前写入 install code
* ``POST /api/zigbee/devices`` — 删除/移除设备（遗留接口；适配器 ``zigbee`` ）

入网窗口
^^^^^^^^

请求体 JSON 可选。 ``seconds`` 缺省 60。 ``0`` 关闭窗口。

.. code-block:: bash

   curl -s -X POST http://HUB/api/zigbee/permit_join \
     -H 'Content-Type: application/json' \
     -d '{"seconds":60}'

Zigbee 入网窗口与 BLE Mesh 配网窗口不能同时打开。若 Mesh 正在配网，permit-join
会失败（错误字符串在 JSON 中）。射频 / 配网状态见 ``GET /api/radio/provision``
（ :doc:`ble_mesh` ）。

Install code
^^^^^^^^^^^^

若入网设备使用 install code，须在 permit-join 之前调用。请求体：

.. code-block:: json

   {
     "ieee": "...",
     "install_code": "..."
   }

两个字段均为字符串。IEEE 为十六进制（可带 ``0x`` 前缀）。

移除设备（遗留）
^^^^^^^^^^^^^^^^

``POST /api/zigbee/devices`` 用于删除/移除设备。若请求体未写 ``adapter`` ，
处理函数会注入 ``zigbee`` 。语义与 Hub 点位移除相同（ ``point_id`` ，或
``delete_all`` 加适配器）。

.. code-block:: json

   { "point_id": "zigbee.zb_0a1b" }

控制
----

灯具走标准 Hub 命令 API（ ``turn_on`` / ``turn_off`` / ``toggle`` ，
或 Devices 页的 ``set`` / ``toggle`` + ``parameter`` ）：

.. code-block:: bash

   curl -s -X POST http://HUB/api/command \
     -H 'Content-Type: application/json' \
     -d '{"point_id":"zigbee.zb_0a1b","cmd":"turn_on"}'

   curl -s -X POST http://HUB/api/devices/control \
     -H 'Content-Type: application/json' \
     -d '{"device":"light","parameter":"power","value":true,"point_id":"zigbee.zb_0a1b"}'

点位身份与厂商扩展
------------------

每台 Zigbee 设备对应 **一条** Host 点（``zigbee.zb_XXXX``）。登记时适配器写入
顶层身份字段（不是类型目录参数 / 设备页滑条）：

* ``props.ieee`` — ``0x`` + 16 位十六进制
* ``props.manufacturer`` / ``props.model`` — 入网访谈后若能读到 Basic
  cluster（0x0000）属性 0x0004 / 0x0005 则写入（尽力而为；读失败不阻断入网）
* ``props.sw_version`` / ``props.hw_version`` — Basic Application Version /
  HW Version（设备未实现对应属性时详情为空）
* ``props.category`` — Zigbee Host **不** 填写。缺省时 UI 用类型目录标签
  （``light`` →「灯」）。厂商插件可 merge 产品名作为品类。

遥测更新只通过 ``minihub_point_merge_params`` 合并 ``props.params``，因此同一
点上的插件字段会保留。

厂商插件（L2）按 ``props.manufacturer`` 匹配（本地过滤，如打包的
``catalog.json``），写在同一 ``point_id`` 上：

* **认领** — 写入 ``props.owner`` = 插件 ``adapter`` 名。该槽在
  ``adapters.json`` 为 ``enabled`` 时，总览把卡片挪到厂商分组（文案见
  ``meta.json`` ``filter_label``）。**不需要** 云端目录。
* **OTA 标记** — 仅在 GET ``catalog_url`` 成功后 merge ``props.ota``
  （``available``、``version``，可选 ``url`` / ``file`` / ``size``）。示例::

     {
       "ota": {
         "available": true,
         "version": "1.2.3",
         "url": "https://vendor.example/fw.bin",
         "file": "/data/ota/7c2c67fffe55d918.bin",
         "size": 524288
       }
     }

启动终端设备 OTA（ZCL 0x0019）可用适配器命令 ``ota`` / ``start_ota``
（``params`` 中可选 ``path`` / ``file_path`` / ``url``），或 Host
``minihub_zigbee_ota_offer(point_id_or_ieee, file_path)``。在 SDK OTA Upgrade
服务端路径完全接通之前，这些接口返回 ``ESP_ERR_NOT_SUPPORTED`` 并打清晰日志 —
插件现在仍可发布 ``props.ota``。

Cluster 映射（新设备类别）
--------------------------

Zigbee 接线（分类 / 属性 → ``props.params`` / 控制命令）是**表驱动**的。
文案与控件仍在类型目录（:doc:`type_catalog`）；映射文件只回答 ZCL cluster
如何接到参数。

默认文件（编入固件）::

   components/minihub_zigbee/config/zigbee_cluster_map.json

运行时叠加（在嵌入默认之后合并；同身份条目后者覆盖）：

* ``/store/preinstall/zigbee_cluster_map.json`` — Store / 出厂预装
* ``/data/config/zigbee_cluster_map.json`` — 设备上覆盖

新增 Zigbee 类别且不必改 ``zigbee_adapter.c`` 的 if-else：

1. 在 ``devices.csv`` / ``parameters.csv`` 增加类型与参数
2. 扩展 ``zigbee_cluster_map.json`` （或 overlay）：

   * ``classify`` — HA device id 和/或 cluster CAP 名 → MiniHub ``type``
   * ``attrs`` — ``cluster`` + ``attr`` → 参数名、``access``、``report``
   * ``controls`` — 参数 → ``on_off`` / ``move_to_level`` / ``write_attr``

仍需 C 实现的通用 ZCL 写路径：On/Off（已有）、属性写（已有）、Level Control
``move_to_level`` （映射可先写，栈暂返回 ``NOT_SUPPORTED``）。新的
``report`` 解码 kind 也需要一小段栈/适配器解码。

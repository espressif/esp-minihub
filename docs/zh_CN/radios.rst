射频与接入
==========

:link_to_translation:`en:[English]`

Host **静态适配器** 把射频 / IP 设备变成 Core 点位。厂商逻辑（认领、云目录、
OTA 标记）走 Store 插件，写在 **同一** ``point_id`` 上，不要为同一物理设备
再登记第二个点。

**Thread 不是设备适配器：** 只做 OpenThread Border Router。Matter-over-Thread
终端出现在 Matter 点上。MQTT / Modbus 只有配置块，见 :doc:`data_model`。

身份、认领、OTA
================

总览卡片不展示身份芯片（``manufacturer`` / ``uuid`` / ``node_id`` 等）。
详情页展示厂商 / 型号 / 版本。认领后若插件槽 ``enabled``，卡片从射频分组挪到
``props.owner`` 对应分组；命令仍走 ``point.adapter`` （射频 Host）。

.. list-table::
   :header-rows: 1
   :widths: 12 22 24 22 20

   * - 适配器
     - Hub 点
     - 身份（Host 写）
     - 空中升级
     - 演示插件
   * - :doc:`zigbee`
     - ``zigbee.zb_XXXX`` 一台一
       点
     - Basic 0x0004/0x0005 等
     - 桩（``NOT_SUPPORTED``）
     - ``examples/plugins/zigbee_ota``
   * - :doc:`ble_mesh`
     - ``light/bm_XXXX`` 一台一
       点
     - Composition CID/PID/VID；
       ``uuid``
     - 桩（Firmware Update /
       BLOB）
     - ``examples/plugins/ble_mesh_ota``
   * - :doc:`matter`
     - ``light/matter_n{node}_ep{ep}``
       **按 endpoint**
     - EP0 Basic Information；
       ``node_id``
     - CSA Provider ``0x0029`` +
       BDX
     - ``examples/plugins/matter_ota``
   * - :doc:`thread`
     - **无** 设备点
     - —
     - 走 Matter 点
     - 不要做 ``thread_ota``

认领（``props.owner``）**不需要** 云端目录。``props.ota`` 仅在 GET
``catalog_url`` 成功后 merge。演示插件用 ``MINIHUB_PLUGIN_DIRS`` 打包，
**不要** 写进公开 Host ``store_preinstall``。

终端 OTA REST：``POST /api/devices/ota`` （:doc:`api/host_rest`）。Hub
**自身** 固件升级是另一条路径：:doc:`ota`。

L1 不要往 ``zigbee`` / ``matter`` / ``ble_mesh`` 的 ``devices[]`` 里造纸面设备，
也不要给 Thread 发明设备列表。见 :doc:`plugins/develop_l1`。

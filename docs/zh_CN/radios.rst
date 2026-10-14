射频与接入
==========

:link_to_translation:`en:[English]`

Host **静态适配器** 把射频 / IP 设备变成 Core 点位。厂商逻辑（认领、云目录、
OTA 标记）走 Store 插件，写在 **同一** ``point_id`` 上，不要为同一物理设备
再登记第二个点。

**Thread 不是设备适配器：** 只做 OpenThread Border Router。Matter-over-Thread
终端出现在 Matter 点上。MQTT / Modbus 只有配置块，见 :doc:`data_model`。

Wi-Fi STA 是 Host 适配器（``adapters.wifi``）。以太网 **不是** 适配器，仍由
``minihub_net_init()`` 拉起。2.4 GHz 三组（Wi-Fi、IEEE 802.15.4 的 Zigbee
**或** Thread、BLE Mesh）**最多同时开两组**。三组都 ``enabled`` 时保留
Wi-Fi + 802.15.4，跳过 BLE Mesh。Zigbee 与 Thread 绝不同时 start（固件也不能
同时链接；JSON 双开则跳过 Thread）。``minihub_adapter_start_all()`` 按
Wi-Fi、802.15.4、BLE Mesh 再其它适配器的顺序 start。互斥不改写
``adapters.json``。

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
   * - wifi
     - **无** 设备点（STA 上行）
     - —
     - —
     - —
   * - :doc:`zigbee`
     - ``zigbee.zb_XXXX`` 一台一
       点
     - Basic 0x0004/0x0005 等
     - 桩（``NOT_SUPPORTED``）
     - —
   * - :doc:`ble_mesh`
     - ``ble_mesh.bm_XXXX`` 一台一
       点
     - Composition CID/PID/VID；
       ``uuid``
     - 桩（Firmware Update /
       BLOB）
     - —
   * - :doc:`matter`
     - ``{transport}.matter_n{node}_ep{ep}``
       **按 endpoint**
     - EP0 Basic Information；
       ``node_id``
     - CSA Provider ``0x0029`` +
       BDX
     - —
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

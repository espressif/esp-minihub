射频与接入总览
==============

:link_to_translation:`en:[English]`

Host **静态适配器** 把射频 / IP 流量变成 Core 点位。开发者逻辑（关联、云目录、OTA
标记）由商店插件承载，写在 **同一** ``point_id`` 上。一台物理设备就是一条点；再登记
第二个点，在 Core 看来就是第二台设备。

Thread 不是设备适配器。它只做 OpenThread Border Router，所以 Matter-over-Thread
端设备出现在 Matter 点上。MQTT / Modbus 只有配置块，见 :doc:`data_model`。

稀缺的是 2.4 GHz 射频。Wi-Fi 与 BLE Mesh 在同一颗射频上共存；IEEE 802.15.4
（Zigbee **或** Thread）与这两者都不共存，Zigbee 与 Thread 也彼此排斥。所以一个
产品要么跑 Wi-Fi / BLE Mesh 这一组，要么只跑一个 802.15.4 栈 —— 做 Zigbee 或
Thread 的 Hub，IP 上行就得走以太网。

``minihub_adapter_start_all()`` 按 Wi-Fi、802.15.4、BLE Mesh 的顺序 start；不占
射频的适配器（如 Modbus）排在最后。Wi-Fi 一组优先： ``adapters.json`` 同时要求
Wi-Fi 或 BLE Mesh 与 Zigbee/Thread 时，输的是 802.15.4 那个适配器 —— 不会被
init、不会 start、不产生任何点、也不会被 poll，日志中出现 ``radio mutex`` 相关
WARN，且不回写那个文件。

Zigbee 与 Thread 可以一起编进固件。哪个启动由 ``adapters.json`` 里该项的
``enabled`` 决定；两项都为 true 时，按启动顺序跳过后一个（注册顺序里 Zigbee 在
Thread 之前）。两个 Kconfig 选项之间没有 ``depends on`` / ``select`` 依赖。

Wi-Fi STA 是 Host 适配器（ ``adapters.wifi`` ）。以太网由 ``minihub_net_init()``
启动，本身不是适配器，也不参与射频竞争。

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - 已启用的射频组合
     - 结果
   * - Wi-Fi + BLE Mesh
     - 正常（唯一支持的共存组合）
   * - Wi-Fi + Zigbee
     - **丢弃 Zigbee**
   * - Wi-Fi + Thread
     - **丢弃 Thread**
   * - BLE Mesh + Zigbee
     - **丢弃 Zigbee**
   * - BLE Mesh + Thread
     - **丢弃 Thread**
   * - Zigbee + Thread
     - **丢弃后启动的那个**（Zigbee 先注册，所以是 Thread）
   * - Wi-Fi + BLE Mesh + Zigbee
     - **丢弃 Zigbee**，Wi-Fi + BLE Mesh 保留

身份、关联、OTA
----------------

总览卡片不展示身份信息（``manufacturer`` / ``uuid`` / ``node_id`` 等）。
详情页展示厂商 / 型号 / 版本。关联后若插件槽 ``enabled``，卡片从射频分组移到
``props.owner`` 对应分组；命令仍走 ``point.adapter`` （射频 Host）。

.. list-table::
   :header-rows: 1
   :widths: 14 28 30 28

   * - 适配器
     - Hub 点
     - 身份（Host 写）
     - 空中升级
   * - wifi
     - **无** 设备点（STA 上行）
     - —
     - —
   * - :doc:`zigbee`
     - ``zigbee.zb_XXXX`` 一台一点
     - Basic 0x0004/0x0005 等
     - 桩（``NOT_SUPPORTED``）
   * - :doc:`ble_mesh`
     - ``ble_mesh.bm_XXXX`` 一台一点
     - Composition CID/PID/VID；
       ``uuid``
     - 桩（Firmware Update /
       BLOB）
   * - :doc:`matter`
     - ``{transport}.matter_n{node}_ep{ep}``
       **按 endpoint**
     - EP0 Basic Information；
       ``node_id``
     - CSA Provider ``0x0029`` +
       BDX
   * - :doc:`thread`
     - **无** 设备点
     - —
     - 走 Matter 点

仓库内没有任何射频演示插件：关联与 OTA 逻辑作为自己的插件作用于
同一个 ``point_id``。Thread 也不需要 —— 它的端设备就是 Matter 点，
不要做 ``thread_ota`` 插件。

关联（``props.owner``）与 OTA 就绪块（``props.ota``）都是 **插件侧约定**: Host
代码不写这两个键，也不会替插件去拉厂商目录。什么时候算「有固件可升」由插件自己
判断（用它自己的 ``adapters.json`` 配置项、NVS 状态或定时抓取）。插件用
``MINIHUB_PLUGIN_DIRS`` 打包，
**不要** 写进公开 Host ``store_preinstall``。

端设备 OTA REST：``POST /api/devices/ota`` （:doc:`api/host_rest`）。Hub
**自身** 固件升级是另一条路径：:doc:`ota`。

JSON 不要往 ``zigbee`` / ``matter`` / ``ble_mesh`` 的 ``devices[]`` 里编造纸面设备，
也不要给 Thread 发明设备列表。见 :doc:`plugins/develop_json`。

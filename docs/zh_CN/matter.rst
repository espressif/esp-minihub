Matter
========

:link_to_translation:`en:[English]`

MiniHub 使用 `乐鑫 Matter SDK <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_
作为 **Matter 控制器** （非简单终端设备示例）。Matter 位于 RF 之上（Wi-Fi /
以太网 / Thread）。设备条目在 ``adapters.matter`` 下，可选 per-device
``transport``：``wifi``、``ethernet``、``thread``。

上游文档：

* `SDK 开发指南 <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html>`_
* `Matter Controller <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/matter_controller.html>`_
* `ESP-IDF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_

构建
----

使能 ``CONFIG_MINIHUB_MATTER_ADAPTER`` ，并导出 ``ESP_MATTER_PATH`` 后直接编译；
脚本发现该选项为 ``=y`` 会自动带上 Matter。需要 Matter-over-Thread 时再使能
``CONFIG_MINIHUB_THREAD_ADAPTER`` （与 Zigbee 互斥）。

.. code-block:: bash

   export ESP_MATTER_PATH=/path/to/esp-matter
   ./tools/build_firmware.py -app minihub build

合成 sdkconfig 时会带上 ``matter.defaults`` 、 ``matter_over_eth.defaults`` 、
``matter_ble.defaults`` ；开 Thread 时另带 ``thread.defaults`` 。

Thread 侧 dataset 见 ``GET/POST /api/thread/dataset`` ，再配网 Thread 终端。

运行时配置
----------

在 ``/data/config/adapters.json`` 中启用（出厂配置：
``application/minihub/config/adapters.json``）：

.. code-block:: json

   {
     "adapters": {
       "matter": {
         "enabled": true,
         "devices": [
           {
             "node_id": 2,
             "point_id": "matter.light.2",
             "endpoint_id": 1,
             "transport": "wifi"
           }
         ]
       }
     }
   }

JSON 字段见 :ref:`adapters-json` 。

REST 接口
---------

* ``GET /api/matter/status`` — 控制器就绪 / 配网状态（JSON）
* ``POST /api/matter/commission`` — 仅启动配网
* ``POST /api/matter/devices`` — 可选配网 + 写入 ``adapters.json``

配网（仅配对）
^^^^^^^^^^^^^^

**网络发现** （设备已在局域网 / multi-admin）：

.. code-block:: bash

   curl -s -X POST http://HUB/api/matter/commission \
     -H 'Content-Type: application/json' \
     -d '{"node_id":2,"setup_pin":20202021,"discovery":"network"}'

**BLE + 设备 Wi-Fi** （首次；须 ``pairing_code``，不可用 ``setup_pin``）：

.. code-block:: json

   {
     "node_id": 2,
     "pairing_code": "MT:...",
     "discovery": "ble_wifi",
     "wifi_ssid": "MyHome",
     "wifi_password": "secret"
   }

**响应**

.. code-block:: json

   { "status": "started" }

错误时 ``status`` 为 ``ESP_ERR_*`` 名称；可选 ``message`` 字符串。

discovery 取值（``discovery`` 字段）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Web 添加对话框始终使用 BLE + DNS-SD（CHIP ``DiscoveryType::kAll``）：

* ``ble_thread``（关闭 Wi-Fi 开关时的默认）— BLE + 注入 Hub Thread 数据集
* ``ble_wifi`` / ``first_time`` — BLE + 注入 **设备** Wi-Fi 凭据

``POST /api/matter/commission`` 仍可接受 ``network``（仅 DNS-SD / multi-admin）和
``ble_ethernet``；概览对话框不再提供这两项。

配网并持久化设备
^^^^^^^^^^^^^^^^

``POST /api/matter/devices`` 在提供 ``pairing_code`` 时**立刻 upsert** 设备并写入
``commission_phase: "waiting"``（概览显示 **等待配网中**），再启动配网，并立即返回
``commission: "started"``，**不会**阻塞 HTTP 处理线程。成功后清除 phase，卡片变为
正常设备；失败或 180 秒看门狗超时则保留卡片为 ``commission_phase: "failed"``
（可删除）。用 ``GET /api/matter/status`` 轮询 ``commission``：``idle`` |
``running`` | ``ok`` | ``failed``。省略 ``pairing_code`` 时仍只 upsert、不配网
（``commission: "skipped"``）。

.. code-block:: json

   {
     "pairing_code": "MT:...",
     "node_id": 2,
     "endpoints": [1],
     "discovery": "ble_thread",
     "transport": "thread",
     "merge": true,
     "reload": true
   }

* ``node_id`` — 省略或 < 2 时自动分配下一 id
* ``endpoints`` — 数字数组或逗号字符串 ``"1,2"``；默认 ``[1]``
* ``merge`` — 默认 true；与现有 ``devices[]`` 合并
* ``reload`` — 默认 true；upsert / 收尾后异步热重载 Hub
* ``delete`` — 为 true 时按 ``point_id`` 删除（同 points/remove）

**立即响应（配网已启动）**

.. code-block:: json

   {
     "result": "ok",
     "node_id": 2,
     "synced": 1,
     "reloaded": true,
     "commission": "started"
   }

**成功之后**：``GET /api/matter/status`` 的 ``commission`` 变为 ``ok``，概览卡片
去掉「等待配网中」。

**删除示例**

.. code-block:: json

   { "delete": true, "point_id": "matter.light.2", "reload": true }

控制
----

CASE 会话成功后，通过标准 Hub 命令 API 控制：

.. code-block:: bash

   curl -s -X POST http://HUB/api/command \
     -H 'Content-Type: application/json' \
     -d '{"point_id":"matter.light.2","cmd":"turn_on"}'

当前支持：OnOff 集群（``turn_on`` / ``turn_off`` / ``toggle``）。

点位身份与厂商扩展
------------------

每个已配网 Matter **endpoint** 对应 **一条** Host 点（现有
``{transport}.matter_n{node}_ep{ep}``——``transport`` 为 ``thread`` /
``wifi`` / ``ethernet``——或 ``adapters.json`` 里的 ``point_id``）。
本次不把多 endpoint 合并成「一 node 一点」。登记后 Host 读 EP0 Basic
Information（cluster ``0x0028``，尽力而为；失败不阻断配网）：

* ``props.node_id`` — 十进制控制器 node id（对齐 Zigbee ``ieee`` / Mesh ``uuid``）
* ``props.manufacturer`` / ``props.model`` — VendorName / ProductName
* ``props.sw_version`` — SoftwareVersionString；没有字符串时用 SoftwareVersion
  十进制
* ``props.hw_version`` — HardwareVersionString 或 HardwareVersion
* ``props.category`` — Matter Host **不** 填写。缺省时 UI 用类型目录标签
  （``light`` →「灯」）

身份只 ``minihub_point_merge_props``，**不** 写进 ``adapters.json``。同一
node 的多个 endpoint 点合并同一份 Basic Information。

厂商插件（L2/L3）按 ``props.manufacturer`` 匹配，写在同一 ``point_id`` 上：

* **认领** — ``props.owner`` = 插件 ``adapter`` 名（演示
  ``examples/plugins/matter_ota``）。**不需要** 云端目录。
* **OTA 标记** — 仅在 GET ``catalog_url`` 成功后 merge ``props.ota``。

启动 OTA 用适配器命令 ``ota`` / ``start_ota``（``path`` / ``file_path`` /
``url``），或 Host ``minihub_matter_ota_offer``。Hub 作为 CSA **OTA
Provider**（cluster ``0x0029``）：向终端 **OTA Requestor**（``0x002A``
EP0）发 ``AnnounceOTAProvider``，QueryImage 成功后走 **BDX**。镜像须为
Matter OTA 文件（CSA 头 ``0x1BEEF11E``）。本地文件由 Hub 在 loopback HTTP
上供给 BDX 下载器；``http(s)`` URL 则由 BDX 直接拉取。需要
``CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER`` 与
``CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED``（``matter.defaults`` 已打开）。
Matter-over-Thread 终端也走 **Matter** 点，见 :doc:`thread`。

``MINIHUB_PLUGIN_DIRS`` 含 ``examples/plugins`` 时才打包 ``matter_ota``；
不要写进公开 Host ``store_preinstall``。

Wi-Fi 由谁管理？
----------------

MiniHub 控制器构建上 **关闭** CHIP Wi-Fi STA （``matter_over_eth.defaults``）。
存在凭据时 MiniHub 仍通过 ``minihub_net_start_wifi()`` 连接 Host STA。
默认路由按 IPv4 上行选择 netif — 见 :doc:`networking`。

术语
----

使用 **Matter 控制器** （ESP-Matter 用语）。**配网** = 将设备加入 fabric。
见 :doc:`faq`。

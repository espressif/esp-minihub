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
``application/minihub/configs/adapters.json``）：

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

* ``GET /api/matter/status`` — 控制器就绪 / 配网状态，并按 node 给出 ``subscriptions``
  活性（JSON）
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

* ``ble_thread`` （关闭 Wi-Fi 开关时的默认）— BLE + 注入 Hub Thread 数据集
* ``ble_wifi`` / ``first_time`` — BLE + 注入 **设备** Wi-Fi 凭据

``POST /api/matter/commission`` 仍可接受 ``network``（仅 DNS-SD / multi-admin）和
``ble_ethernet``；概览对话框不再提供这两项。

配网并持久化设备
^^^^^^^^^^^^^^^^

``POST /api/matter/devices`` 在提供 ``pairing_code`` 时**立刻 upsert** 设备并写入
``commission_phase: "waiting"`` （概览显示 **等待配网中**），再启动配网，并立即返回
``commission: "started"``，**不会** 阻塞 HTTP 处理线程。成功后清除 phase，卡片变为
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

当前支持：OnOff 集群（``turn_on`` / ``turn_off`` / ``toggle``）。这次写入是乐观值，设备
自己的值怎么回来见 :ref:`matter-state-reporting` 。

.. _matter-state-reporting:

状态上报
--------

``/api/command`` 在 OnOff 命令交给 CHIP 的那一刻就把请求值写进点位，所以卡片显示的是
Hub 想要什么，而不是设备最终是什么。设备自身的状态经属性订阅（attribute subscription）
上报。

Matter 适配器为每个已配网 node 建立 **一条** 订阅，覆盖该 node 在 ``adapters.json``
里所有 endpoint 上的 OnOff 属性（cluster ``0x0006`` ，attribute ``0x0000``）。订阅在
适配器 ``start`` 时建立；配网把「配网中」行变成正常设备时（适配器 ``resync``）再建一次；
订阅断开后由适配器 ``poll`` 重建。每条上报都会把对应 endpoint 的点位写成 ``on`` 或
``off`` ，但绝不覆盖 ``commission_phase`` 仍置位的行，也不会把这样的行标成在线。

上报出口只有一个回调位：``minihub_matter_set_attr_cb()`` 。Matter 适配器在 ``init`` 时
登记且从不清除，所以目前它是唯一的消费者。

怎么看上报有没有到
^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   curl -s http://HUB/api/matter/status

.. code-block:: json

   {
     "ready": true,
     "last_error": "subscribe connect failed: Error CHIP:0x00000032",
     "subscriptions": [
       { "node_id": 2, "endpoints": [1], "live": true, "reports": 3 }
     ]
   }

两种构建配置都会有 ``subscriptions`` ，stub 构建里它是空数组。上面的数字只是字段形状，
不是台架读数。

* ``live`` — 该 node 的订阅已建立。活性是按 **node** 统计而非按 endpoint ：ESP-Matter 的
  建立 / 终止回调只带 ``(node_id, subscription_id)`` ，同一 node 的所有 endpoint 一起生、
  一起死。
* ``reports`` — 该 node 收到的布尔上报条数。当前只订阅了 OnOff ，所以实际收到的就是
  OnOff 上报，但计数本身不按 cluster 过滤。它是按 node 累计的，只有当该 node 的行被拆除
  （``minihub_matter_drop_node``）或被重新分配给别的 node 时才清零；给一个非 live 的 node
  重新建立订阅并不清零。判断「上报到了」要看它，不要看 ``value`` ：
  ``minihub_point_set()`` 会忽略没变的值，所以 ``value`` 分不清「设备报了一次 off」和
  「什么都没发生」。
* ``last_error`` — 只保留最近一条控制器错误，非空时才显示（例如上面的 CHIP 连接错误）。
  它不是按 node 的日志：之后任何一次不相干的成功（OnOff 命令、新的配网）都会覆盖或清空
  它，所以失败的建立只在此之前可见。``poll`` 会重试一直起不来的 node ，但它每分钟才跑一
  次、且是在建立时才给该 node 的退避计数加一，所以用秒表能看到的是：下一分钟重试一次，
  之后大约 2 分钟、4 分钟、8 分钟各一次，最终停在 10 分钟的上限——名义上的 30 秒下限根本
  观测不到。
* endpoint 集合变了的 node 会先被真实拆除（``send_shutdown_subscription``）再重建；已经从
  ``adapters.json`` 删掉的 node 同样回收——重新配网的设备会拿到一个 **新的** node id ，
  否则旧行会一直留着。

台架演示
^^^^^^^^

必须出现的判据（这是验收条件，见下面的状态说明）：

1. ``GET /api/matter/status`` 里该 node 的 ``live`` 为 ``true`` 。
2. 先把卡片复位，免得把 Hub 自己的写入当成上报：``POST /api/hub/reload`` 重新发布配置的
   行；基线值看 ``GET /api/points`` ，基线 ``reports`` 看 ``GET /api/matter/status`` ，
   点位列表里不带 ``reports`` 。
3. **不经过 Hub** 改变设备：设备自己的按键、断电重连、或另一个控制器。观察期间不要调用
   ``/api/command`` 。
4. 再读 ``GET /api/matter/status`` 和 ``GET /api/points`` 。**通过** 的判据是 ``reports``
   变大、且点位值变成设备的真实状态。若值变了而 ``reports`` 不动，那是 Hub 命令写的，
   这一轮不作数。

**还在等一台可达的设备。** 这套流程尚未在任何台架上完整跑通。开发用的这块板子上，已配网
的 node 不响应 CASE ，``live`` 一直是 ``false`` ，``reports`` 一直是 ``0`` ，从未有上报
到达，所以第 3、4 步（包括一条上报多久能落到卡片上）都还没验证。
``components/minihub_matter/matter_ctrl.cpp`` 里的订阅区间配置是
``MATTER_SUB_MIN_INTERVAL`` = 5 秒、``MATTER_SUB_MAX_INTERVAL`` = 30 秒，它们只是限制
设备最多多久报一次，**不是** 从按下按键到卡片更新的实测时延。

限制
^^^^

* **只解析布尔属性。** TLV 解不出 bool 的上报直接丢弃；当前也只订阅了 OnOff。亮度、颜色、
  位置（Cover）都需要各自的解码和按 cluster 的值映射。
* **8 个 node ，每 node 16 个 endpoint。** 订阅表容量是 ``MATTER_SUB_MAX`` = 8 ，第 9 个
  node 不会被接受（``ESP_ERR_NO_MEM`` ，``last_error`` 为 ``subscribe: table full`` ）。
  单 node 的 endpoint 列表上限 16 ，与一次配网的数量上限相同。
* **没有按需复读。** ``minihub_matter_read_onoff()`` 已实现（被接受时返回
  ``ESP_ERR_NOT_FINISHED`` ，值仍走同一个上报回调），但目前没有调用者，也没有周期性重读：
  稳态只靠订阅。订阅一直起不来时，卡片就一直保持最后一次写入的值——来自 Hub 命令，或者
  登记时写的 ``off`` 。
* ``online`` **不代表设备在应答。** 非「配网中」的 Matter 行只要控制器就绪就标为在线，
  所以会出现 online 为真、同时 ``live`` 为假、``reports`` 为 0 、值还是旧值——这正是当前
  设备不可达的样子。信卡片之前先看 ``subscriptions`` 。
* **休眠设备（ICD）未验证。** 它们的订阅需要很长的空闲区间和 active-schedule 决策，
  实测之前按不可用处理。
* **多 fabric** 沿用现有配网代码的 ``get_fabric_index()`` 假设。
* **不订阅事件。** ESP-Matter 的订阅接口带一个事件路径列表和一个事件回调位；MiniHub 的
  订阅两者都不传（事件路径为空、回调是 ``nullptr``），所以现在既不订阅也不投递 Matter
  事件。

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

厂商插件（L2）按 ``props.manufacturer`` 匹配，写在同一 ``point_id`` 上：

* **认领** — ``props.owner`` = 插件 ``adapter`` 名。**不需要** 云端目录。
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

``MINIHUB_PLUGIN_DIRS`` 含该插件目录时才打包。

Wi-Fi 由谁管理？
----------------

MiniHub 控制器构建上 **关闭** CHIP Wi-Fi STA （``matter_over_eth.defaults``）。
存在凭据时 MiniHub 仍通过 ``minihub_net_start_wifi()`` 连接 Host STA。
默认路由按 IPv4 上行选择 netif — 见 :doc:`networking`。

术语
----

使用 **Matter 控制器** （ESP-Matter 用语）。**配网** = 将设备加入 fabric。
见 :doc:`faq`。

Thread
======

:link_to_translation:`en:[English]`

MiniHub 的 ``thread`` 适配器是 **OpenThread Border Router** （运营数据集 /
与 Zigbee **互斥** 的 802.15.4 射频），**不是** 设备适配器。Thread 独占 2.4 GHz
射频 —— Wi-Fi 与 BLE Mesh 都不能与它并列，所以 Thread 的 Hub 走以太网接入局域网。
总表见 :doc:`radios`。

Border Router 负责转发、并不拥有设备，所以这个适配器 **不** 登记 Hub 设备点，也
**没有** ``devices[]``，``handle_command`` 返回不支持。Matter-over-Thread 终端配网后
出现在 ``adapters.matter`` 下（``transport=thread``），厂商、关联、OTA 也都在那里，
见 :doc:`matter`。

使能 ``CONFIG_MINIHUB_THREAD_ADAPTER`` （与 Zigbee 互斥）。这个选项同时
``select`` 了 ``MINIHUB_MATTER_ADAPTER``，所以 Thread 构建一定带着 Matter 控制器。
dataset 走 ``GET`` / ``POST /api/thread/dataset``；当前角色、信道与
``dataset_set`` 由 ``GET /api/thread/status`` 报出 —— 出厂 JSON 里那个
``adapters.thread.channel`` 没有任何代码读它。字段见 :doc:`data_model`
（:ref:`adapters-json`）。

启停 Border Router（无请求体；鉴权与其它写接口相同）。仅在打开上述选项时存在：

* ``POST /api/thread/start`` — JSON ``status`` 为 ``ok``，或 ``esp_err`` 名，
  失败时可能带 ``message``
* ``POST /api/thread/stop`` — 同样的 ``status``；协议栈本来就没开时也是 ``ok``

用 L1 往 Thread 注入 ``devices[]``，只会造出没人能通信的纸面设备；
``thread_ota`` 插件也没有任何东西可升级，所以不要发布这样的 pack。这是约定，
不是强制限制：校验器把 ``thread`` 列为可注入的 Host 适配器，Host 的 pack
加载器也会把设备合并进 ``adapters.thread``，而 Border Router 直接忽略它们。
这样的 pack 照常加载，只是没有任何东西可通信。

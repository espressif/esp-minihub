Thread
======

:link_to_translation:`en:[English]`

MiniHub 的 ``thread`` 适配器是 **OpenThread Border Router** （dataset / 信道 /
与 Zigbee **互斥** 的 802.15.4 射频），**不是** 设备适配器。总表见 :doc:`radios`。

它 **不** 登记 Hub 设备点，也 **没有** ``devices[]``。``handle_command`` 返回
不支持。Matter-over-Thread 终端配网后出现在 ``adapters.matter`` 下（
``transport=thread``）；厂商、认领、OTA 都走 **Matter** 点，见 :doc:`matter`。

使能 ``CONFIG_MINIHUB_THREAD_ADAPTER`` （与 Zigbee 互斥）。dataset：
``GET`` / ``POST /api/thread/dataset``。字段见 :doc:`data_model`
（:ref:`adapters-json`）。

启停 Border Router（无请求体；鉴权与其它写接口相同）。仅在打开上述选项时存在：

* ``POST /api/thread/start`` — JSON ``status`` 为 ``ok``，或 ``esp_err`` 名，
  失败时可能带 ``message``
* ``POST /api/thread/stop`` — 同样的 ``status``；协议栈本来就没开时也是 ``ok``

不要用 L1 往 Thread 注入设备列表；也不要做 ``thread_ota`` 插件。

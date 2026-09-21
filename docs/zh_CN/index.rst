MiniHub 编程指南
================

:link_to_translation:`en:[English]`

**MiniHub** 是面向多协议物联网中枢的 ESP32 Host 固件：Web UI、REST、
WebSocket 事件、Store 动态模块（``.mhub`` ），以及 Zigbee / BLE Mesh /
Matter / MQTT / Modbus / Thread 等适配器。

第三方生态以 **Store 插件** 形式交付，而不是 Host Core 依赖。
本指南的目标是：读完即可理解架构与优势，并完成量产部署，以及
**不阅读 Core C 源码** 即可二次开发 Host 固件与 Store 插件。

建议阅读顺序：先 :doc:`getting_started` 烧起来，再分层与点位模型，然后
:doc:`radios` 看各射频如何登记点、认领与 OTA。

上游平台文档（此处不重复展开）：

* `ESP-IDF 编程指南 <https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/>`_
* `ESP-Matter 编程指南 <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_

.. toctree::
   :maxdepth: 2
   :caption: 上手与模型

   introduction
   getting_started
   architecture
   architecture_overview
   type_catalog
   data_model
   rules
   networking

.. toctree::
   :maxdepth: 2
   :caption: 射频与接入

   radios
   matter
   thread
   zigbee
   ble_mesh

.. toctree::
   :maxdepth: 2
   :caption: 产品与现场

   ota
   application/index
   host_customization
   web_console
   production

.. toctree::
   :maxdepth: 2
   :caption: 插件与 API

   plugins/index
   api/index

.. toctree::
   :maxdepth: 1
   :caption: 附录

   vibe_coding
   faq
   contributing

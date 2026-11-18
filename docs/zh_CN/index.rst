MiniHub 编程指南
================

:link_to_translation:`en:[English]`

.. image:: ../_static/hero/hero.zh_CN.svg
   :align: center
   :alt: MiniHub —— 把多种协议设备聚成一台 Hub 的 Host 固件

MiniHub 是跑在 ESP32-S31 上的物联网中枢 Host 固件。一份固件集成 Web 控制台、REST
API、WebSocket 事件、本机规则引擎以及 :doc:`radios` 中所列适配器；产品无需在中枢
之外另配控制器或网关。

厂商云与 OEM 代码不编进固件。它们在运行时以商店模块装进来，或者作为
``application/`` 下的独立产品构建。

自动化面向的是 :ref:`点位（point） <point>`，不是协议。一台设备就是一条点，所以设备换了射频，
规则照样有效。

从哪里入手
----------

.. list-table::
   :header-rows: 1
   :widths: 26 30 44

   * - 你想做什么
     - 先读
     - 然后
   * - 判断 MiniHub 是否适合你的产品
     - :doc:`introduction`
     - :doc:`architecture`、:doc:`radios`、:doc:`faq`
   * - 编译并烧录通用 Host
     - :doc:`getting_started`
     - :doc:`networking`、:doc:`web_console`、:doc:`ota`
   * - 做自己的产品（板级、分区、配置）
     - :doc:`application/index`
     - :doc:`host_customization`、:doc:`production`
   * - 写一个商店模块
     - :doc:`plugins/first_plugin`
     - :doc:`plugins/overview`，再读你选的那一层
   * - 用你自己的 App 或云去驱动 Hub
     - :doc:`api/host_rest`
     - :doc:`data_model`、:doc:`rules`、:doc:`data_flow`
   * - 增加设备类型或换一套控制台外观
     - :doc:`type_catalog`
     - :doc:`web_console`、:doc:`data_flow`

本指南固定用法的词（点位、适配器、商店、层级、ABI）收在 :doc:`glossary`；
仓库目录逐层说明见 :doc:`source_tree`。

上游平台文档此处不重复展开：

* `ESP-IDF 编程指南 <https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/>`_
* `ESP-Matter 编程指南 <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_

.. toctree::
   :maxdepth: 2
   :caption: 先读这里

   introduction
   glossary
   getting_started
   source_tree

.. toctree::
   :maxdepth: 2
   :caption: Hub 如何工作

   architecture
   data_flow
   type_catalog
   data_model
   rules
   networking
   time
   limits

.. toctree::
   :maxdepth: 2
   :caption: 射频与接入

   radios
   matter
   thread
   zigbee
   ble_mesh
   rainmaker

.. toctree::
   :maxdepth: 2
   :caption: 产品与现场

   ota
   application/index
   host_customization
   kconfig
   tools
   web_console
   production
   versions
   security

.. toctree::
   :maxdepth: 2
   :caption: 插件与 API

   plugins/index
   api/index

.. toctree::
   :maxdepth: 1
   :caption: 附录

   vibe_coding
   troubleshooting
   faq
   contributing

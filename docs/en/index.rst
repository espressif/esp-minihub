MiniHub Programming Guide
=========================

:link_to_translation:`zh_CN:[中文]`

**MiniHub** is ESP32 Host firmware for multi-protocol IoT hubs: Web UI, REST,
WebSocket events, Store dynamic modules (``.mhub``), and adapters
(Zigbee / BLE Mesh / Matter / MQTT / Modbus / Thread).

Third-party ecosystems ship as **Store plugins**, not Host Core dependencies.
This guide is written so you can **understand the architecture, ship a product,
and develop Host firmware or Store plugins without reading Core C sources**.

Read :doc:`getting_started` first to flash a build, then the layering and
point model, then :doc:`radios` for how each radio registers points, claim,
and OTA.

Upstream platform docs (not duplicated here):

* `ESP-IDF Programming Guide <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_
* `ESP-Matter Programming Guide <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_

.. toctree::
   :maxdepth: 2
   :caption: Start and model

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
   :caption: Radios and access

   radios
   matter
   thread
   zigbee
   ble_mesh

.. toctree::
   :maxdepth: 2
   :caption: Product and field

   ota
   application/index
   host_customization
   web_console
   production

.. toctree::
   :maxdepth: 2
   :caption: Plugins and APIs

   plugins/index
   api/index

.. toctree::
   :maxdepth: 1
   :caption: Appendix

   vibe_coding
   faq
   contributing

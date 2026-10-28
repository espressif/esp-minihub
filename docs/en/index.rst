MiniHub Programming Guide
=========================

:link_to_translation:`zh_CN:[中文]`

.. image:: ../_static/hero/hero.en.svg
   :align: center
   :alt: MiniHub — host firmware for a multi-protocol device hub

MiniHub is Host firmware that turns an ESP32-class SoC into a hub for devices
that speak several protocols at once. One firmware build carries the web console, the
REST API, WebSocket events, the rules engine and the adapters listed in
:doc:`radios`, so a product needs no separate controller or gateway beside it.

Vendor clouds and OEM code are not
compiled into the firmware. They arrive at run time as Store modules, or they
live in a product tree under ``application/``.

Automation talks to :ref:`points <point>` rather than to protocols. One device is one point, and
a rule keeps working after that device moves to a different radio.

Where to start
--------------

.. list-table::
   :header-rows: 1
   :widths: 30 34 36

   * - What you want to do
     - Start here
     - Then
   * - Decide whether MiniHub fits your product
     - :doc:`introduction`
     - :doc:`architecture`, :doc:`radios`, :doc:`faq`
   * - Build and flash the default Host
     - :doc:`getting_started`
     - :doc:`networking`, :doc:`web_console`, :doc:`ota`
   * - Ship your own product (board, partitions, config)
     - :doc:`application/index`
     - :doc:`host_customization`, :doc:`production`
   * - Write a Store module
     - :doc:`plugins/first_plugin`
     - :doc:`plugins/overview`, then the tier you chose
   * - Drive a Hub from your own app or cloud
     - :doc:`api/host_rest`
     - :doc:`data_model`, :doc:`rules`, :doc:`data_flow`
   * - Add a device type or reskin the console
     - :doc:`type_catalog`
     - :doc:`web_console`, :doc:`data_flow`

Words that this guide uses in a fixed sense (point, adapter, Store, tier, ABI)
are collected in :doc:`glossary`. The repository layout, directory by
directory, is in :doc:`source_tree`.

Upstream platform docs are not duplicated here:

* `ESP-IDF Programming Guide <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_
* `ESP-Matter Programming Guide <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_

.. toctree::
   :maxdepth: 2
   :caption: Start here

   introduction
   glossary
   getting_started
   source_tree

.. toctree::
   :maxdepth: 2
   :caption: How the Hub works

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
   :caption: Radios and access

   radios
   matter
   thread
   zigbee
   ble_mesh
   rainmaker

.. toctree::
   :maxdepth: 2
   :caption: Product and field

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
   :caption: Plugins and APIs

   plugins/index
   api/index

.. toctree::
   :maxdepth: 1
   :caption: Appendix

   vibe_coding
   troubleshooting
   faq
   contributing

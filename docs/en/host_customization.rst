Host Customization
==================

:link_to_translation:`zh_CN:[中文]`

Use this chapter to ship your own Host firmware product on top of MiniHub Core
(OEM / secondary development). Layout of ``application/`` and the generic
``minihub`` product: :doc:`application/index` · :doc:`application/minihub`.

Overview
--------

A **product** is a folder ``application/APP/``. It selects:

* Board and partition table
* Sdkconfig feature set (Ethernet, Matter, Store, …)
* Default ``adapters.json`` / rules / scenes
* Optional factory-preinstalled Store modules

Approach
--------

1. Copy ``application/minihub`` to ``application/myhub`` (steps below).
2. Under ``myhub``, edit product files (board, partitions, factory JSON, sdkconfig
   features, and so on) and add your own code.
3. **Keep Core shared** in repo ``components/minihub_*`` — do not copy those
   libraries into ``myhub``.
4. You usually leave the copied ``main.c`` alone; stacks come from Kconfig and
   generated ``adapters_setup.c``.

Factory flash and OTA: :doc:`production`.

Step-by-step: create ``myhub``
------------------------------

1. **Copy the generic Host as a template**

   .. code-block:: bash

      cp -a application/minihub application/myhub

2. **Edit** ``application/myhub/manifest.yml``

   The copy already has a full ``sdkconfig_features`` list. Drop unused items
   (``rtsp_cam``, ``usb_msc``, …) to save flash. Minimal example:

   .. code-block:: yaml

      extends: _host_base
      board: esp32_s31_function_coreboard_1
      partition_table: partitions.csv
      store_preinstall: []
      sdkconfig_features:
        - common
        - memory
        - partitions_16m
        - eth
        - wifi_sta
        - dynamic_modules

3. **Edit** ``application/myhub/configs/sdkconfig/app.yml``

   ``features:`` must match the manifest ``sdkconfig_features`` (compose uses
   ``app.yml``).

4. **Factory JSON** (optional)

   * ``application/myhub/configs/adapters.json``
   * ``application/myhub/configs/rules.json``, ``scenes.json``

   If a file is missing under the product, the build falls back to repo
   ``configs/*.json``.

   Type labels and widgets are **not** product JSON. They live in Host
   ``components/minihub_device_widget/model/*.csv``. Add a type there, or
   ``PUT /api/dev/catalog/…`` on a running Hub. Device instances still
   register one point each (:doc:`data_model`; catalog: :doc:`type_catalog`).

5. **Add your own code**

   Add sources under ``application/myhub/main/`` (or a product component) and
   list them in ``CMakeLists.txt`` ``SRCS``. You usually leave bring-up as-is;
   stacks come from Kconfig.

   The Web sidebar version (``v`` plus the string) is Kconfig
   ``MINIHUB_PRODUCT_VERSION`` (default ``0.1.0``). Git describe stays on the
   system page as the commit line, not as this product version.

6. **Build and flash**

   .. code-block:: bash

      ./tools/build_firmware.py -app myhub build
      ./tools/build_firmware.py -app myhub -p PORT app-flash monitor

Product file tree (typical)
---------------------------

.. code-block:: text

   application/myhub/
     manifest.yml
     partitions.csv
     configs/
       adapters.json
       rules.json
       scenes.json
       sdkconfig/
         app.yml
     main/
       CMakeLists.txt
       main.c
       adapters_setup.h
       idf_component.yml              # generated
       adapters_setup.generated.c   # generated
     boards/                          # optional board overrides

Partitions
----------

The generic Host ``partitions.csv`` is a starting point. Plan your own table
when a product has extra needs — for example Zigbee ``zb_storage`` / ``zb_fct``,
RainMaker ``fctry``, OTA slot size, ``storage`` / ``modstore``.

Web console
-----------

The console HTML is staged on ``modstore`` as ``/store/preinstall/web/``.
To skin ``index.html`` / ``login.html`` or push files with curl:
:doc:`web_console`. The generic ``CMakeLists.txt`` copies
``components/minihub_web/web/`` only (no ``application/APP/web/`` overlay).

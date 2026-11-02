Getting Started
===============

:link_to_translation:`zh_CN:[中文]`

Requirements
------------

* ESP-IDF 6.2 with ESP32-S31 support, at the revision recorded below
* Python and the build toolchain — follow the ESP-IDF installation guide:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html#installation
* PyYAML (``pip install pyyaml``) — required by the build and store-packaging helper
  scripts
* Matter (optional; required when ``CONFIG_MINIHUB_MATTER_ADAPTER`` is enabled):
  ``ESP_MATTER_PATH`` pointing at a local `esp-matter` checkout

.. _minihub-toolchain-pin:

Pinned toolchain
----------------

ESP-IDF is not carried inside this repository (no submodule, no gitlink), so the
revision actually used during development is stated here instead. Build against
anything else and an ESP32-S31 failure may simply be a toolchain mismatch.

.. list-table::
   :header-rows: 1
   :widths: 22 48 30

   * - Component
     - Revision in use
     - Why it matters
   * - ESP-IDF
     - ``master`` at ``434d5fd100b90a03c19018c572809cc29c7db28b``, which
       ``git describe`` reports as ``qa-test-full-v6.2-20261016-70-g434d5fd100b``
       (ESP-IDF 6.2.0 plus 70 commits)
     - ESP32-S31 support is newer than any 6.2 release tag; IDF 5.x has no S31
       toolchain at all
   * - Target
     - ``esp32s31``, always with ``--preview``
     - The target is still preview, so ``idf.py`` refuses it without the flag

To pin your own build to this exact ESP-IDF instead of guessing:

.. code-block:: bash

   git clone -b master https://github.com/espressif/esp-idf.git
   cd esp-idf && git checkout 434d5fd100b90a03c19018c572809cc29c7db28b
   ./install.sh esp32s31 && . ./export.sh

``tools/build_firmware.py`` reads ``$IDF_PATH`` and otherwise uses the
``esp-idf/`` folder next to the repository (gitignored). A checkout elsewhere
therefore needs ``export IDF_PATH=...``, or the build silently uses a different
ESP-IDF than the one recorded above.

Clone and first build
---------------------

From the repository root:

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build
   ./tools/build_firmware.py -app minihub -p /dev/ttyUSB0 app-flash monitor

``build_firmware.py`` wraps ``idf.py``: it composes sdkconfig, syncs manifest,
applies board defaults, and invokes the IDF build under ``application/minihub/``.
If ``-app`` is another product (for example ``my_app``), the IDF build runs under
``application/my_app/``.

The build writes the standard IDF artifacts under ``application/minihub/build/`` —
the flashable application image is ``application/minihub/build/minihub.bin``, with
``bootloader/``, ``partition_table/``, and ``flasher_args.json`` alongside it.

Default product (minihub)
-------------------------

The generic Host in ``application/minihub/`` uses
``application/minihub/configs/sdkconfig/app.yml``:
Ethernet-first uplink (Wi-Fi STA fallback), USB MSC, RTSP, Modbus, Store
dynamic modules, and more. Reference board: ``esp32_s31_function_coreboard_1``.

After flash: expected boot
--------------------------

Typical serial lines (search these strings):

1. ``MiniHub starting...``
2. ``storage mounted at /data`` (then ``modstore`` at ``/store``)
3. ``uplinks prepared: wifi=… eth=… (wifi STA deferred)`` — Ethernet started; STA not connected yet
4. ``HTTP server on port 80`` — Web comes up before radio stacks
5. Adapter ``start`` (Zigbee / BLE Mesh / … as built)
6. ``eth got IP`` or ``wifi got IP`` (the latter after ``minihub_net_start_wifi()``)
7. ``MiniHub ready (net=… prov=… web_ui=… lcd_ui=… eth=… wifi=…)``

``MiniHub ready`` means features are prepared, not that an IPv4 address is assigned.

.. image:: ../_static/boot/boot_timeline.en.svg
   :align: center
   :alt: MiniHub power-on sequence: firmware → storage → uplinks → web → adapters → IP → ready

``HUB`` in the URLs throughout this guide is the address printed by ``eth got IP``
or ``wifi got IP`` (for example ``192.168.1.50``). The guide does not register a
hostname.

Open ``http://HUB/`` for the Web UI (you may be redirected to ``/login``). On
first visit, log in as ``admin`` and set a password of at least 6 characters,
then use the Web UI to verify the device and operate it. Skinning the HTML:
:doc:`web_console`.

When ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` is on (default), the Web UI prompts for
login; REST clients must send a session cookie or ``Authorization: Bearer``.
Optional token: ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` (which wins when
non-empty; NVS ``api_token`` is read only if that string is empty).
See :doc:`faq`.

Network logs: see :doc:`networking` (``eth got IP``, ``wifi got IP``).

Product model
-------------

.. code-block:: text

   application/APP/          Product (board, manifest, factory JSON)
   components/               Shared Core + adapters
   configs/                   Shared manifest base + sdkconfig fragments
   cmake/generated/          Sync output (gitignored)

* Select product: ``-app APP`` or ``MINIHUB_APP=APP`` (default ``minihub``).
  Details: :doc:`application/index`.
* Manifest: ``application/APP/manifest.yml`` (usually ``extends: _host_base``)
* Sdkconfig: ``application/APP/configs/sdkconfig/app.yml``

After manifest edits, run ``./tools/build_firmware.py -app APP build`` (merge
happens automatically before compile).

Compose sdkconfig: ``tools/compose_sdkconfig.py``. Stack list:
``cmake/generated/sdkconfig_stacks.gen``.

Directory by directory, including what the build generates: :doc:`source_tree`.

Develop a Store plugin (quick path)
-----------------------------------

1. Add ``examples/plugins/ID/`` (or your own tree via
   ``MINIHUB_PLUGIN_DIRS``; see
   :doc:`plugins/develop_l1` or :doc:`plugins/develop_l2`)
2. ``python3 tools/pack_store_module.py pack ID``
3. Run ``python3 tools/store_server.py`` on the PC (the Hub pulls packages over
   LAN; do not run this on the device)
4. Open the Web UI at ``http://HUB/`` and open ``Store``
5. Set the custom Store URL to that server, then refresh the catalog and
   install ``ID`` in the Store UI (L1 packs use ``pack`` as the type)

.. only:: not hide_l3

   L3 native modules: :doc:`plugins/develop_l3`. Compile the ``.so`` before pack.

New type / widgets: Host ``model/*.csv`` (rebuild) or upload both CSVs via REST
on a running Hub (``PUT /api/dev/catalog/…``; no Web page -- :doc:`type_catalog`).
Demos:
L1 ``examples/plugins/ble_mesh_pack``, L2 ``color_bulb_js`` / ``meter_plug_js``.

.. only:: not hide_l3

   There is no in-tree L3 demo; scaffold one with
   ``tools/new_store_module.py --tier l3 --id my_native``.

OEM Host product: see :doc:`host_customization`.

Matter build
------------

Enable ``CONFIG_MINIHUB_MATTER_ADAPTER``, export ``ESP_MATTER_PATH``, then build:

.. code-block:: bash

   export ESP_MATTER_PATH=/path/to/esp-matter
   ./tools/build_firmware.py -app minihub build

See :doc:`matter` for Matter controller networking and REST APIs.

Where to read next
------------------

* **Vibe coding** — :doc:`vibe_coding`
* **Understand the Hub** — :doc:`architecture`, then
  :doc:`data_flow`, then :doc:`type_catalog` and :doc:`data_model`
* **Networking** — :doc:`networking`
* **Radios and access** — :doc:`radios` (Matter / Thread BR / Zigbee / BLE Mesh)
* **Hub firmware update** — :doc:`ota`
* **Understand** ``application/`` **then OEM Host** — :doc:`application/index` then
  :doc:`host_customization` then :doc:`production`
* **Replace the Web console** — :doc:`web_console`
* **Write a plugin** — :doc:`plugins/overview`
* **Call APIs** — :doc:`api/host_rest` · :doc:`api/mcp` · :doc:`api/index`

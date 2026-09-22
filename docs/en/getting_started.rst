Getting Started
===============

:link_to_translation:`zh_CN:[中文]`

Requirements
------------

* ESP-IDF ~6.2 with ESP32-S31 support
* Python 3.10+ and PyYAML (``pip install pyyaml``)
* Matter (optional; required when ``CONFIG_MINIHUB_MATTER_ADAPTER`` is enabled):
  ``ESP_MATTER_PATH`` pointing at a local `esp-matter` checkout

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

Default product (minihub)
-------------------------

The generic Host in ``application/minihub/`` uses ``config/sdkconfig/app.yml``:
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

Open ``http://HUB/`` for the Web UI (you may be redirected to ``/login``). REST
base: ``http://HUB/api/``. First visit: username ``admin``, set a password of
at least 6 characters. Skinning the HTML: :doc:`web_console`.

First checks (replace ``HUB`` with the IPv4 from ``eth got IP`` or ``wifi got IP``):

.. code-block:: bash

   curl -s http://HUB/api/
   curl -s http://HUB/api/system
   curl -s http://HUB/api/points

When ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` is on (default), these return HTTP 401
until you send a session cookie or ``Authorization: Bearer``. Optional token:
``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` or NVS ``api_token``. See :doc:`faq`.

Network logs: see :doc:`networking` (``eth got IP``, ``wifi got IP``).

Product model
-------------

.. code-block:: text

   application/APP/          Product (board, manifest, factory JSON)
   components/               Shared Core + adapters
   config/                   Shared manifest base + sdkconfig fragments
   cmake/generated/          Sync output (gitignored)

* Select product: ``-app APP`` or ``MINIHUB_APP=APP`` (default ``minihub``).
  Details: :doc:`application/index`.
* Manifest: ``application/APP/manifest.yml`` (usually ``extends: _host_base``)
* Sdkconfig: ``application/APP/config/sdkconfig/app.yml``

After manifest edits, run ``./tools/build_firmware.py -app APP build`` (merge
happens automatically before compile).

Compose sdkconfig: ``tools/compose_sdkconfig.py``. Stack list:
``cmake/generated/sdkconfig_stacks.gen``.

Develop a Store plugin (quick path)
-----------------------------------

1. Add ``examples/plugins/ID/`` (or a vendor tree via
   ``MINIHUB_PLUGIN_DIRS``; see
   :doc:`plugins/develop_l1`, :doc:`plugins/develop_l2`, or
   :doc:`plugins/develop_l3`)
2. ``python3 tools/pack_store_module.py pack ID``
3. Run ``python3 tools/store_server.py`` on the PC (the Hub pulls packages over
   LAN; do not run this on the device)
4. Hub: ``POST /api/store/config`` with ``custom_url`` pointing at that server
5. ``POST /api/store/refresh`` then ``POST /api/store/install`` with
   ``{"kind":"module","id":"ID"}`` (L1 packs use ``"kind":"pack"``) or Web UI →
   Store

New type / widgets: Host ``model/*.csv`` (rebuild) or
``PUT /api/dev/catalog/…`` on a running Hub (:doc:`type_catalog`). Demos:
L1 ``examples/plugins/ble_mesh_pack``, L2 ``color_bulb_js``, L3
``color_bulb`` / ``meter_plug``.

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
  :doc:`architecture_overview`, then :doc:`type_catalog` and :doc:`data_model`
* **Networking** — :doc:`networking`
* **Radios and access** — :doc:`radios` (Matter / Thread BR / Zigbee / BLE Mesh)
* **Hub firmware update** — :doc:`ota`
* **Understand** ``application/`` **then OEM Host** — :doc:`application/index` then
  :doc:`host_customization` then :doc:`production`
* **Replace the Web console** — :doc:`web_console`
* **Write a plugin** — :doc:`plugins/overview`
* **Call APIs** — :doc:`api/host_rest` · :doc:`api/mcp` · :doc:`api/index`

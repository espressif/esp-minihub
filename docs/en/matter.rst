Matter
======

:link_to_translation:`zh_CN:[中文]`

MiniHub uses `Espressif's SDK for Matter <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/>`_
as a **Matter controller** (commissioner), not as a simple end-device sample. Matter
sits above RF (Wi-Fi / Ethernet / Thread). Devices live under ``adapters.matter``
with optional per-device ``transport``: ``wifi``, ``ethernet``, or ``thread``.

Upstream:

* `Developing with the SDK <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html>`_
* `Matter Controller <https://docs.espressif.com/projects/esp-matter/en/latest/esp32/matter_controller.html>`_
* `ESP-IDF <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/>`_

Build
-----

Enable ``CONFIG_MINIHUB_MATTER_ADAPTER``, export ``ESP_MATTER_PATH``, then build;
the script pulls in Matter when that option is ``=y``. For Matter-over-Thread
also enable ``CONFIG_MINIHUB_THREAD_ADAPTER`` (mutually exclusive with Zigbee).

.. code-block:: bash

   export ESP_MATTER_PATH=/path/to/esp-matter
   ./tools/build_firmware.py -app minihub build

Compose then includes ``matter.defaults``, ``matter_over_eth.defaults``, and
``matter_ble.defaults``; Thread adds ``thread.defaults``.

Thread dataset: ``GET/POST /api/thread/dataset``, then commission Thread end
devices.

Runtime configuration
---------------------

Enable in ``/data/config/adapters.json`` (factory JSON: ``application/minihub/config/adapters.json``):

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

JSON fields: :ref:`adapters-json`.

REST endpoints
--------------

* ``GET /api/matter/status`` — commissioner ready / pairing state (JSON)
* ``POST /api/matter/commission`` — start pairing only
* ``POST /api/matter/devices`` — commission (optional) + upsert into ``adapters.json``

Commission (pairing only)
^^^^^^^^^^^^^^^^^^^^^^^^^

**Network discovery** (device already on LAN / multi-admin):

.. code-block:: bash

   curl -s -X POST http://HUB/api/matter/commission \
     -H 'Content-Type: application/json' \
     -d '{"node_id":2,"setup_pin":20202021,"discovery":"network"}'

**BLE + device Wi-Fi** (first-time; requires ``pairing_code``, not ``setup_pin``):

.. code-block:: json

   {
     "node_id": 2,
     "pairing_code": "MT:...",
     "discovery": "ble_wifi",
     "wifi_ssid": "MyHome",
     "wifi_password": "secret"
   }

**Response**

.. code-block:: json

   { "status": "started" }

On error: ``status`` is an ``ESP_ERR_*`` name; optional ``message`` string.

Discovery values (``discovery`` field)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``network`` (default) — multi-admin / already on LAN; ``setup_pin`` allowed
* ``ble_wifi`` / ``first_time`` — Hub BLE + inject **device** Wi-Fi credentials
* ``ble_thread`` — BLE commission onto Thread (needs Thread BR build)
* ``ble_ethernet`` / ``ethernet`` — BLE commission for Ethernet device

Commission + persist devices
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``POST /api/matter/devices`` commissions when ``pairing_code`` is present, then
upserts device entries and optionally reloads the hub.

.. code-block:: json

   {
     "pairing_code": "MT:...",
     "node_id": 2,
     "endpoints": [1],
     "discovery": "ble_wifi",
     "wifi_ssid": "MyHome",
     "wifi_password": "secret",
     "transport": "wifi",
     "merge": true,
     "reload": true
   }

* ``node_id`` — omit or < 2 to auto-allocate next id
* ``endpoints`` — array of numbers or comma string ``"1,2"``; default ``[1]``
* ``merge`` — default true; merge with existing ``devices[]``
* ``reload`` — default true; call ``minihub_hub_reload`` after upsert
* ``delete`` — when true, remove by ``point_id`` instead (same as points/remove)

**Success response**

.. code-block:: json

   {
     "result": "ok",
     "node_id": 2,
     "synced": 1,
     "reloaded": true,
     "commission": "started"
   }

**Delete example**

.. code-block:: json

   { "delete": true, "point_id": "matter.light.2", "reload": true }

Control
-------

After CASE session succeeds, control lights via standard Hub command API:

.. code-block:: bash

   curl -s -X POST http://HUB/api/command \
     -H 'Content-Type: application/json' \
     -d '{"point_id":"matter.light.2","cmd":"turn_on"}'

Supported today: OnOff cluster (``turn_on`` / ``turn_off`` / ``toggle``).

Point identity and vendor extras
--------------------------------

Each commissioned Matter **endpoint** is **one** Host point (existing
``light/matter_n{node}_ep{ep}`` or the ``point_id`` in ``adapters.json``).
This round does **not** collapse endpoints into one point per node. After
register, Host reads EP0 Basic Information (cluster ``0x0028``, best-effort;
commission is not blocked on failure):

* ``props.node_id`` — decimal controller node id (like Zigbee ``ieee`` / Mesh
  ``uuid``)
* ``props.manufacturer`` / ``props.model`` — VendorName / ProductName
* ``props.sw_version`` — SoftwareVersionString, else decimal SoftwareVersion
* ``props.hw_version`` — HardwareVersionString or HardwareVersion
* ``props.category`` — **not** filled by Matter Host. UI falls back to the
  type-catalog label (``light`` → “灯”)

Identity uses ``minihub_point_merge_props`` only — **not** written into
``adapters.json``. Every endpoint of the same node gets the same Basic
Information.

Vendor plugins match ``props.manufacturer`` on that ``point_id``:

* **Claim** — ``props.owner`` = plugin ``adapter`` name (demo
  ``examples/plugins/matter_ota``). Cloud catalog is **not** required.
* **OTA flag** — only after GET ``catalog_url`` succeeds, merge ``props.ota``.

Start OTA with adapter commands ``ota`` / ``start_ota`` (``path`` /
``file_path`` / ``url``), or Host ``minihub_matter_ota_offer``. The Hub is a
CSA **OTA Provider** (cluster ``0x0029``): it sends ``AnnounceOTAProvider`` to
the device **OTA Requestor** (``0x002A`` EP0), then **BDX** after QueryImage.
The image must be a Matter OTA file (CSA header ``0x1BEEF11E``). Local files
are served on loopback HTTP for the BDX downloader; ``http(s)`` URLs are
fetched directly. Requires ``CONFIG_CHIP_DEVICE_ENABLE_DYNAMIC_SERVER`` and
``CONFIG_ESP_MATTER_OTA_PROVIDER_ENABLED`` (on in ``matter.defaults``).
Matter-over-Thread end-devices are **Matter** points; see :doc:`thread`.

``.mhub`` packing needs ``MINIHUB_PLUGIN_DIRS``; do **not** add
``matter_ota`` to public Host ``store_preinstall``.

Who owns Wi-Fi?
---------------

CHIP Wi-Fi station is **off** on MiniHub controller builds
(``matter_over_eth.defaults``). MiniHub still connects Host STA via
``minihub_net_start_wifi()`` when credentials exist. Default-route selection only
picks the netif with IPv4 — see :doc:`networking`.

Terminology
-----------

Use **Matter controller** (ESP-Matter). **Commissioning** = pairing a device to the
fabric. See :doc:`faq`.

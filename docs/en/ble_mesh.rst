BLE Mesh
========

:link_to_translation:`zh_CN:[中文]`

MiniHub runs BLE Mesh as a **Host adapter**. Devices live under
``adapters.ble_mesh``. JSON field details (including ``provision.uuid_filters``
and ``provision.bind_models``) are in :doc:`data_model` (:ref:`adapters-json`).

Enable ``CONFIG_MINIHUB_BLE_MESH_SDK`` in ``sdkconfig`` / menuconfig, then build
(the default stacks already include BLE Mesh; a disabled option is dropped):

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build

REST paths below are compiled when ``CONFIG_MINIHUB_BLE_MESH_SDK`` is enabled.
Bring-up and IP uplink: :doc:`getting_started`, :doc:`networking`.

Coexistence
-----------

The Zigbee permit-join window and the BLE Mesh provision window cannot both be
open. Firmware returns error strings such as Zigbee join in progress, or BLE
Mesh already provisioning. Poll ``GET /api/radio/provision`` for RF / provision
status.

Runtime configuration
---------------------

Enable in ``/data/config/adapters.json``:

.. code-block:: json

   {
     "adapters": {
       "ble_mesh": {
         "enabled": true,
         "provision": {},
         "devices": []
       }
     }
   }

Set ``adapters.ble_mesh.enabled``. Provisioning policy lives under
``provision`` (``uuid_filters`` / ``bind_models``). Bound nodes are in
``devices[]``. Entries with ``type`` ``sensor`` are skipped. Fields:
:ref:`adapters-json`. L1 packs may **merge** extra bind/filter defaults
(demo ``examples/plugins/ble_mesh_pack``); they must not invent
``devices[]``.

REST endpoints
--------------

Overview: :doc:`api/host_rest`.

* ``POST /api/ble_mesh/permit_provision`` — open or close the provision window
* ``POST /api/ble_mesh/devices`` — remove devices (legacy; adapter ``ble_mesh``)
* ``GET /api/radio/provision`` — RF / provision coexistence status

Permit provision
^^^^^^^^^^^^^^^^

Optional JSON body. ``seconds`` defaults to 60. ``0`` closes the window.

.. code-block:: bash

   curl -s -X POST http://HUB/api/ble_mesh/permit_provision \
     -H 'Content-Type: application/json' \
     -d '{"seconds":60}'

Remove devices (legacy)
^^^^^^^^^^^^^^^^^^^^^^^

``POST /api/ble_mesh/devices`` removes devices. If the body omits ``adapter``,
the handler injects ``ble_mesh``. Same remove semantics as Hub points remove.

.. code-block:: json

   { "point_id": "mesh.light.1" }

Radio / provision status
^^^^^^^^^^^^^^^^^^^^^^^^

``GET /api/radio/provision`` returns JSON including whether Zigbee / BLE Mesh /
Matter are compiled in, whether Zigbee permit-join or BLE Mesh provision is
active, and a ``busy`` flag for RF-critical windows. Use it before opening
another join or provision window.

Control
-------

After a node is bound, use ``POST /api/command`` with the Hub ``point_id``
(OnOff models: ``turn_on`` / ``turn_off`` / ``toggle``). See :doc:`api/host_rest`.

Point identity and vendor extras
--------------------------------

Each BLE Mesh node is **one** Host point (``ble_mesh.bm_XXXX``, same
``<protocol>.<device>`` convention as Zigbee). Legacy ``{type}/bm_XXXX`` ids are
migrated on NVS load. On
register the adapter sets top-level identity (not catalog params / Devices
sliders):

* ``props.uuid`` — Device UUID hex (Mesh has no Zigbee ``ieee``)
* ``props.manufacturer`` — Composition Page 0 **Company ID** (Bluetooth CID).
  ``0x02E5`` becomes ``Espressif``; others ``0xNNNN``. **Not** a ZCL Basic string
* ``props.model`` / ``props.sw_version`` — Product ID / Version ID as ``0xNNNN``
* ``props.category`` — **not** filled by BLE Mesh Host. UI falls back to the
  type-catalog label (``light`` → “灯”). A vendor plugin may merge a product
  name as category

Telemetry (including CTL temperature / lightness) uses
``minihub_point_merge_params`` or a value-only ``point_set``. Do **not** replace
the whole ``props`` object, or uuid / manufacturer / ``owner`` / ``ota`` are
wiped.

Vendor plugins (L2) match ``props.manufacturer`` (local filter, e.g.
packed ``catalog.json``) on that same ``point_id``:

* **Claim** — set ``props.owner`` to the plugin ``adapter`` name. Overview
  moves the card under the vendor section when that adapter slot is
  ``enabled`` (label from ``meta.json`` ``filter_label``). Cloud catalog is
  **not** required for the claim.
* **OTA flag** — only after GET ``catalog_url`` succeeds, merge
  ``props.ota`` (``available``, ``version``, optional ``url`` / ``file`` /
  ``size``). Example::

     {
       "ota": {
         "available": true,
         "version": "1.2.3",
         "url": "https://vendor.example/fw.bin",
         "file": "/data/ota/aabbccddeeff0011.bin",
         "size": 524288
       }
     }

Start Mesh Firmware Update / BLOB with adapter commands ``ota`` or ``start_ota``
(optional ``path`` / ``file_path`` / ``url`` in ``params``), or Host
``minihub_ble_mesh_ota_offer(point_id, file_path)``. Until air transport is
wired, these return ``ESP_ERR_NOT_SUPPORTED`` with a clear log — plugins can
still advertise ``props.ota`` now.

``.mhub`` packing happens only when ``MINIHUB_PLUGIN_DIRS`` includes that
plugin dir.

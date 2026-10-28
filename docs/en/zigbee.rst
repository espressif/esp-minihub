Zigbee
======

:link_to_translation:`zh_CN:[中文]`

MiniHub runs Zigbee as a **Host adapter** (coordinator), not as a standalone
end-device sample. Devices live under ``adapters.zigbee``. JSON field details
are in :doc:`data_model` (:ref:`adapters-json`); this chapter covers build,
join, and REST.

Zigbee takes the 2.4 GHz radio to itself: it shares it with neither Wi-Fi nor BLE
Mesh, and Thread wants the same 802.15.4 radio, so the two stacks are **mutually
exclusive at build time** — ``tools/build_firmware.py`` stops a build that enables
both, since neither Kconfig option depends on the other. JSON that still asks for
both is honoured by skipping Thread; JSON that asks for Zigbee beside Wi-Fi or BLE
Mesh is honoured by skipping Zigbee. A Zigbee Hub therefore reaches the LAN over
Ethernet. See :doc:`getting_started` and :doc:`networking` for Hub bring-up, and
:doc:`radios` for the full matrix.

Build
-----

Enable ``CONFIG_MINIHUB_ZIGBEE_SDK``, then build:

.. code-block:: bash

   ./tools/build_firmware.py -app minihub build

REST paths below are compiled in with that option.

Runtime configuration
---------------------

Enable in ``/data/config/adapters.json`` (factory JSON:
``application/minihub/configs/adapters.json``):

.. code-block:: json

   {
     "adapters": {
       "zigbee": {
         "enabled": true,
         "devices": []
       }
     }
   }

Set ``adapters.zigbee.enabled`` and list bound devices in ``devices[]``.
Field list: :ref:`adapters-json`.

REST endpoints
--------------

Present only when ``CONFIG_MINIHUB_ZIGBEE_SDK`` is compiled in. Auth and
overview: :doc:`api/host_rest`.

* ``POST /api/zigbee/permit_join`` — open or close the join window
* ``POST /api/zigbee/install_code`` — add an install code before join
* ``POST /api/zigbee/devices`` — remove devices (legacy; adapter ``zigbee``)

Permit join
^^^^^^^^^^^

Optional JSON body. ``seconds`` defaults to 60. ``0`` closes the window. Values
above **254** are silently clamped to 254 before they reach the stack — the log
line reports what was actually opened — because the field is one byte.

.. code-block:: bash

   curl -s -X POST http://HUB/api/zigbee/permit_join \
     -H 'Content-Type: application/json' \
     -d '{"seconds":60}'

The Zigbee join window and the BLE Mesh provision window cannot both be open.
The Mesh side blocks it while its **permit window is open**, not only while a
device is actually being provisioned — an open-but-idle Mesh window already
fails permit-join (see error strings in the
JSON body). RF / provision status: ``GET /api/radio/provision``
(:doc:`ble_mesh`).

Install code
^^^^^^^^^^^^

Call before permit-join when the joiner uses an install code. Body:

.. code-block:: json

   {
     "ieee": "...",
     "install_code": "..."
   }

Both fields are strings. IEEE is hex (optional ``0x`` prefix).

Remove devices (legacy)
^^^^^^^^^^^^^^^^^^^^^^^

``POST /api/zigbee/devices`` deletes/removes devices. If the body omits
``adapter``, the handler injects ``zigbee``. Same remove semantics as Hub
points remove (``point_id``, or ``delete_all`` with adapter).

.. code-block:: json

   { "point_id": "zigbee.zb_0a1b" }

Control
-------

Lights use the standard Hub command API (``turn_on`` / ``turn_off`` /
``toggle``, or Devices-page ``set`` / ``toggle`` with ``parameter``):

.. code-block:: bash

   curl -s -X POST http://HUB/api/command \
     -H 'Content-Type: application/json' \
     -d '{"point_id":"zigbee.zb_0a1b","cmd":"turn_on"}'

   curl -s -X POST http://HUB/api/devices/control \
     -H 'Content-Type: application/json' \
     -d '{"device":"light","parameter":"power","value":true,"point_id":"zigbee.zb_0a1b"}'

Point identity and plugin extras
--------------------------------

Each Zigbee device is **one** Host point (``zigbee.zb_XXXX``). On register the
adapter sets top-level identity (not catalog params / Devices sliders):

* ``props.ieee`` — ``0x`` + 16 hex digits
* ``props.manufacturer`` / ``props.model`` — when Basic cluster (0x0000) attrs
  0x0004 / 0x0005 are readable after interview (best-effort; join is not blocked
  if the read fails)
* ``props.sw_version`` / ``props.hw_version`` — Basic Application Version /
  HW Version (detail stays blank if the attribute is unsupported)
* ``props.category`` — **not** filled by Zigbee Host. UI falls back to the
  type-catalog label (``light`` → “灯”). A plugin may merge a product
  name as category.

Telemetry updates merge **only** ``props.params`` via
``minihub_point_merge_params``, so plugin keys on the same point survive.

Plugins (L2) match ``props.manufacturer`` (local filter, e.g.
packed ``catalog.json``) on that same ``point_id``. Both keys below are
**plugin-side conventions**: no Host code writes ``props.owner`` or ``props.ota``,
and the Hub does not fetch a vendor catalog for you.

* **Associate** — the plugin sets ``props.owner`` to its own ``adapter`` name.
  Overview moves the card under the vendor section when that adapter slot is
  ``enabled`` (label from ``meta.json`` ``filter_label``). Nothing here needs a
  cloud catalog.
* **OTA flag** — the plugin merges ``props.ota`` (``available``, ``version``,
  optional ``url`` / ``file`` / ``size``) whenever its own check says a firmware
  update is ready; where that check comes from (the plugin's ``adapters.json`` block naming
  its vendor URL, its own NVS, a scheduled fetch) is the plugin's business.
  Of these keys the Hub only consumes ``file``, ``url`` and ``version`` when
  ``POST /api/devices/ota`` runs; ``available`` and ``size`` are console display
  hints. Example::

     {
       "ota": {
         "available": true,
         "version": "1.2.3",
         "url": "https://vendor.example/fw.bin",
         "file": "/data/ota/7c2c67fffe55d918.bin",
         "size": 524288
       }
     }

Start end-device OTA (ZCL 0x0019) with adapter commands ``ota`` or ``start_ota``
(optional ``path`` / ``file_path`` / ``url`` in ``params``), or Host
``minihub_zigbee_ota_offer(point_id_or_ieee, file_path)``. Until the SDK OTA
Upgrade server path is fully wired, these return ``ESP_ERR_NOT_SUPPORTED`` with
a clear log — plugins can still advertise ``props.ota`` now.

Cluster map (new categories)
----------------------------

Zigbee wiring (classify / attribute → ``props.params`` / control commands) is
**table-driven**. Labels and widgets stay in the type catalog
(:doc:`type_catalog`); the map only answers how ZCL clusters connect to params.

Default file (embedded in firmware)::

   components/minihub_zigbee/config/zigbee_cluster_map.json

Runtime overlays (merged after the embedded default; later wins on the same
entry identity):

* ``/store/preinstall/zigbee_cluster_map.json`` — Store / factory preinstall
* ``/data/config/zigbee_cluster_map.json`` — on-device override

To add a new Zigbee category without editing ``zigbee_adapter.c``:

1. Add the type + parameters to ``devices.csv`` / ``parameters.csv``
2. Extend ``zigbee_cluster_map.json`` (or overlay) with:

   * ``classify`` — HA device ids and/or cluster CAP names → MiniHub ``type``
   * ``attrs`` — ``cluster`` + ``attr`` → param name, ``access``, ``report``
   * ``controls`` — param → ``on_off`` / ``move_to_level`` / ``write_attr``

Generic ZCL writers that still need C (stack): On/Off (exists), attribute write
(exists), Level Control ``move_to_level`` (map entry present; returns
``NOT_SUPPORTED`` until implemented). New ``report`` decode kinds also need a
small stack/adapter decode path.

BLE Mesh
========

:link_to_translation:`zh_CN:[中文]`

MiniHub runs BLE Mesh as a **Host adapter**. Devices live under
``adapters.ble_mesh``. JSON field details (including ``provision.uuid_filters``
and ``provision.bind_models``) are in :doc:`data_model` (:ref:`adapters-JSON`).

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

.. code-block:: JSON

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
``devices[]``. A declaration's ``models[]`` decide its capabilities when
present; without them the ``type`` decides, through the parameters the
capability map knows for that type. A declaration left with no known
capability is skipped and logged. Fields:
:ref:`adapters-JSON`. JSON packs may **merge** extra bind/filter defaults
(demo ``examples/plugins/ble_mesh_pack``); they must not invent
``devices[]``.

Capability map
--------------

Everything the adapter does with a node comes from one table:
``components/minihub_ble_mesh/config/ble_mesh_map.json`` in the firmware
image. It maps *model → capability → type → parameter → command*:

* ``models[]`` — the Composition models the Hub understands, each with its
  capability bit, the client model that talks to it, the ``get`` opcode that
  reads its state back, and the names a ``devices[]`` declaration may use
  (``cap`` and the published ``alias``)
* ``classify[]`` — which capabilities and sensor properties make a node a
  ``light``, a ``switch`` or a sensor type
* ``params[]`` — the parameters a node exposes, with access and default value
* ``controls[]`` — which Mesh message drives which parameter
* ``sensors[]`` — how a sensor property is decoded: octet count, signedness,
  scale, the "value unknown" sentinel and the catalog bounds

The table decides the bind list, the client models the app key is bound to, the
seeded parameters and the commands. Adding a device type is a JSON edit, not a
firmware change. A policy in ``provision.bind_models`` does not replace that
bind list: the Host binds every model the table knows **plus** the models the
policy names, so a JSON pack can add a vendor model without dropping the
built-in ones.

An OEM can overlay the table without rebuilding: put a file of the same shape at
``/store/preinstall/ble_mesh_map.json`` (modstore image) or
``/data/config/ble_mesh_map.json`` (storage partition). A later file replaces
the rows it names and leaves the other built-in rows alone.

``tools/test_ble_mesh_map.py`` fails when the table drifts from the widget
catalog, from the control fields the adapter looks up, from the model names the
repo publishes, or from the Bluetooth SIG characteristic sizes it quotes.

A node whose Composition matches no ``classify[]`` rule gets **no** point. It is
logged and counted as unclassified, never registered as a controllable switch.

Device types
------------

``light``
   OnOff / Level / Lightness / CTL / HSL.

``switch``
   OnOff only.

Not available over BLE Mesh:

* ``intensity`` — the Mesh lighting models have no intensity path. The catalog
  lists it for ``light``, but the Mesh adapter never seeds it and refuses a
  write to it with ``ESP_ERR_NOT_SUPPORTED``.
* ``sensor_contact`` / ``sensor_smoke`` / ``sensor_tamper`` / ``sensor_water`` /
  ``pm25`` / ``speaker`` — the Bluetooth SIG defines no matching sensor
  property and no audio model. Only a vendor model can carry them, and the map
  table cannot invent one.
* ``battery`` — the Generic Battery model is not in the map yet.

State sync
----------

The shipped ``ble_mesh_map.json`` includes a ``report`` block. After binds finish,
each local client model subscribes to that group, and every Server model bound on
a node gets a Config Model Publication Set naming it. A published Status then
lands in the same decode as a Get reply: the generic and light callbacks gate on
the publish event (``ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT``,
``ESP_BLE_MESH_LIGHT_CLIENT_PUBLISH_EVT``) and the sensor callback dispatches on
the opcode alone. A wall switch or socket that publishes OnOff Status therefore
updates the Hub point without another Get.

Get path (bind complete, Set reply, Hub reboot; also the only state sync when
``report`` is absent):

* When model binds finish: one Get per bound model whose ``models[]`` row
  declares a ``get`` opcode, plus a Sensor Descriptor Get for a node carrying a
  Sensor Server.
* After that descriptor answers: it names the property ids, and a single Sensor
  Get with no property operand returns all of them.
* On every Set reply.
* When the Hub reboots, using the ``(model, elem)`` bind list saved with the
  device in NVS (a Sensor Server already listed in that store gets a Sensor Get,
  not another Descriptor).

``ble_mesh_adapter_poll()`` returns without doing anything: the adapter poll
task does not fetch Mesh state. Every parameter the map marks readable has a
Get behind it. Until a Get or a published Status answers, a parameter shows the
``init`` value from the map. Devices provisioned before the bind list existed keep
working for control but stay on ``init`` until they are commanded or
re-provisioned. Nodes already on the network when ``report`` is first enabled
need a re-provision (or a fresh Publication Set) before they publish to the Hub
group.

Default report block:

.. code-block:: json

   "report": { "group": "0xC001", "period_s": 30, "retransmit_count": 2,
               "retransmit_ms": 200, "ttl": 3 }

Without the block, or with an overlay that omits it, the Hub writes no
Publication to any node and no local client model subscribes to a status
group. State sync then uses only the Get path above.

Field values:

* ``period_s`` — must encode exactly in the Model Publication Period octet.
  One-second steps up to 63 s, then tens of seconds, then tens of minutes. A
  value with no exact encoding is logged and refused; it is not rounded.
* ``group`` — must be a group address. A value that
  ``ESP_BLE_MESH_ADDR_IS_GROUP`` rejects leaves reporting off, with one log
  line.
* ``ttl`` — 0–127, or 255 to keep the node's own default.

A node that refuses the period is configured again without one, and then
publishes only on state change.

REST endpoints
--------------

Overview: :doc:`api/host_rest`.

* ``POST /api/ble_mesh/permit_provision`` — open or close the provision window
* ``POST /api/ble_mesh/devices`` — remove devices (legacy; adapter ``ble_mesh``)
* ``GET /api/radio/provision`` — RF / provision coexistence status. This one is
  **not** gated by ``CONFIG_MINIHUB_BLE_MESH_SDK``: it is always registered and
  answers ``false`` for the availability flags of whatever is not built in.

Permit provision
^^^^^^^^^^^^^^^^

Optional JSON body. ``seconds`` defaults to 60. ``0`` closes the window.

The window is not exactly ``seconds`` long: if a provision is still in flight when
it expires, the Hub extends it **once** by 30 s, and on the second expiry aborts the
stuck provision and closes the window. So a 60 s request can hold the radio about
90 s. Also note that BLE Mesh sits in the Wi-Fi group: with
``adapters.ble_mesh.enabled`` and ``adapters.zigbee.enabled`` both true, the Hub
keeps Wi-Fi and BLE Mesh and drops the 802.15.4 adapter at run time, with only a
``adapter 'zigbee' not started (radio mutex)`` log line to show for it
(:doc:`radios`).

.. code-block:: bash

   curl -s -X POST http://HUB/api/ble_mesh/permit_provision \
     -H 'Content-Type: application/json' \
     -d '{"seconds":60}'

Remove devices (legacy)
^^^^^^^^^^^^^^^^^^^^^^^

``POST /api/ble_mesh/devices`` removes devices. If the body omits ``adapter``,
the handler injects ``ble_mesh``. Same remove semantics as Hub points remove.

.. code-block:: JSON

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

Point identity and plugin extras
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
  type-catalog label (``light`` → “灯”). A plugin may merge a product
  name as category

Live params (``props.params.<name>.value``) are seeded at register from the
node's capabilities — Composition for a provisioned node, the declared type for
a ``devices[]`` entry — with the defaults ``params[]`` quotes (``power``,
``brightness``, ``cct``, ``hue``, ``saturation``), and updated read-modify-write
on telemetry and on ``set`` commands (``brightness`` → Lightness/Level,
``hue``/``saturation`` → HSL Set, ``cct`` → CTL). Do **not** replace the whole
``props`` object, or uuid / manufacturer / ``owner`` / ``ota`` / other params
are wiped.

Plugins (JS) match ``props.manufacturer`` (local filter, e.g.
packed ``catalog.json``) on that same ``point_id``:

* **Associate** — set ``props.owner`` to the plugin ``adapter`` name. Overview
  moves the card under the vendor section when that adapter slot is
  ``enabled`` (label from ``meta.json`` ``filter_label``). Both ``owner`` and the
  OTA block below are **plugin-side conventions** — no Host code writes them, and
  no cloud catalog is required for the associate.
* **OTA flag** — the plugin merges
  ``props.ota`` (``available``, ``version``, optional ``url`` / ``file`` /
  ``size``) whenever its own check finds a firmware update ready; of those, the Hub only
  consumes ``file`` / ``url`` / ``version`` on ``POST /api/devices/ota``.
  Example::

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

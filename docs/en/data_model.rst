Data model
==========

:link_to_translation:`zh_CN:[中文]`

This chapter covers the **runtime point model** and Hub config
(``adapters.json``, scenes). Rules: :doc:`rules`. Device types, widgets, and
icons for the Devices page are in :doc:`type_catalog`.

What the model looks like
-------------------------

MiniHub Core works on **points**, not on C structs for lamps or switches:

* **One physical device = one** ``minihub_point_t`` (Matter is **one
  endpoint** — see :doc:`matter`)
* ``point_id`` = ``protocol.device`` (e.g. ``sim.color_bulb``,
  ``zigbee.zb_7d4b``, ``thread.matter_n2_ep1``)
* Live parameters live on that point under ``props.params.<name>``
* ``type`` is a string (``light`` / ``switch`` / …) — **not** a closed Core enum
* **How the UI draws** comes from the Host **type catalog** (CSV),
  protocol-agnostic — see :doc:`type_catalog`

.. code-block:: text

   Physical device
      │  adapter / plugin registers
      ▼
   point_id = protocol.device     type = "light" | "switch" | …
      │
      ├─ value          optional summary (often power on/off)
      ├─ online
      └─ props.params
            ├─ power       { value, access, … }
            ├─ brightness  { value, access, min, max, … }
            └─ …

   Host type catalog (CSV)              Plugin instance (devices.json)
   which params, labels, widgets, bounds   which params this device uses
          \                              /
           UI = catalog ∩ live props.params

Do **not** split one device into many points (wrong: ``sim.color_bulb.brightness``).

Live point example (``GET /api/points``):

.. code-block:: json

   {
     "point_id": "sim.color_bulb",
     "type": "light",
     "adapter": "color_bulb",
     "value": "on",
     "online": true,
     "props": {
       "manufacturer": "Espressif",
       "category": "Color Bulb",
       "model": "Bulb-42",
       "sw_version": "1.2.0",
       "hw_version": "3",
       "ota": { "available": false },
       "params": {
         "power": { "value": true, "access": "RW" },
         "brightness": { "value": 80, "access": "RW", "max": 80 }
       }
     }
   }

Device identity (top-level ``props``, not catalog ``params``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Register these on the Host point when the device is created (or merge later).
Overview cards do not show identity chips (live controls stay in ``props.params``).
In **device-detail**, ``manufacturer`` / ``model`` / versions / OTA stay blank
when missing; the **品类** row always has a label (``category`` or
``TYPE_LABEL[type]``).

================  ============================================================
Key               Meaning and who writes it
================  ============================================================
``manufacturer``  Vendor / brand. Detail row 「厂商」. Zigbee: Host reads
                  Basic 0x0004. BLE Mesh: Composition CID (``0x02E5`` →
                  ``Espressif``, else ``0xNNNN``). Matter: Basic Information
                  VendorName. Plugins that own the
                  point set it on ``register``.
``category``      Product display name (detail subtitle, card badge).
                  Widgets still join the type catalog by ``type``. If
                  absent, the UI uses ``TYPE_LABEL[type]`` (e.g. ``light``
                  → “灯”). Aliases: ``product`` / ``product_name``.
``model``         Model id. Zigbee: Basic 0x0005. BLE Mesh: Composition PID
                  as ``0xNNNN``. Matter: Basic Information ProductName.
``ieee``          Radio address (Zigbee ``0x`` + 16 hex digits). Written
                  by the Host radio adapter; used by plugins to match.
                  BLE Mesh uses ``uuid`` (Device UUID hex) instead.
                  Matter uses ``node_id`` (decimal).
``sw_version``    Software / application version. Zigbee: Basic Application
                  Version. BLE Mesh: Composition VID as ``0xNNNN``. Matter:
                  Basic Information SoftwareVersionString.
``hw_version``    Hardware version. Zigbee: Basic HW Version.
``owner``         After a vendor plugin claims the device, Overview moves
                  the card from the radio section to that adapter **if**
                  the plugin slot is ``enabled`` in ``adapters.json``.
                  Do **not** change ``point.adapter`` (commands still hit
                  Zigbee / Mesh).
``ota``           Object: ``available`` (bool), optional ``version`` /
                  ``url`` / ``file`` / ``status``. **Only Store plugins**
                  set this. ``POST /api/devices/ota`` prefers
                  ``props.ota.file`` as ``path``, else ``url``. Host
                  adapters do not invent OTA availability.
================  ============================================================

Built-in adapters fill only fields the **protocol** can determine (e.g.
Zigbee Basic ManufacturerName / ModelIdentifier / ApplicationVersion /
HWVersion; BLE Mesh Composition CID/PID/VID and ``uuid``; Matter Basic
Information and ``node_id``). Category label, OTA, and ``owner`` come from plugins via
``minihub_point_register`` / ``minihub_point_merge_props``. Demos
``color_bulb`` / ``meter_plug`` / ``color_bulb_js`` set
``manufacturer=Espressif`` and a product ``category`` on register.

``minihub_point_set`` with a new ``props`` object publishes ``point.updated``
(WebSocket ``GET /ws/events``). Limits: :doc:`api/minihub_core`.

Adapters that publish live parameters should call
``minihub_point_merge_params`` (or ``minihub_point_merge_props``) so other
top-level keys on the same point — ``ota``, ``ieee``, ``uuid``, ``node_id``, ``manufacturer``,
``category``, ``model``, ``sw_version``, ``hw_version``, ``owner``, … — are preserved. Plugins enrich
the **same** ``point_id``; they must not register a second point for the
same physical device.

Where files live
----------------

Factory JSON at **build** time (first existing file wins; repo ``configs/`` is
fallback only):

1. ``application/APP/configs/`` plus the file name
2. else repo ``configs/`` plus the same name (only if step 1 is missing)

On device after first boot the live copies are:

* ``/data/config/adapters.json``
* ``/data/config/rules.json``
* ``/data/config/scenes.json``
* ``/data/config/devices.csv`` — flattened **type catalog** (labels / widgets)
* ``/data/config/model/`` — optional uploaded ``devices.csv`` + ``parameters.csv``

Edit live JSON via Web UI or REST (``PUT /api/config/adapters``,
``POST /api/rules/import``, ``POST /api/scenes/import``), then
``POST /api/hub/reload`` when the API does not reload for you.

Remote catalog CSV updates and column details: :doc:`type_catalog`.

.. _adapters-json:

``adapters.json``
-----------------

The root object is only ``adapters``. Each key is an adapter name and must match
a statically linked adapter in the firmware, or a Store module ``adapter`` field.
Unknown keys are ignored.

Top-level example:

.. code-block:: json

   {
     "adapters": {
       "matter": {
         "enabled": true,
         "devices": []
       },
       "zigbee": { "enabled": false, "devices": [] },
       "ble_mesh": { "enabled": false, "provision": {}, "devices": [] },
       "thread": { "enabled": false, "dataset": null, "channel": null },
       "mqtt": { "enabled": false, "broker": "127.0.0.1", "port": 1883, "devices": [] },
       "modbus": { "enabled": false, "poll_ms": 2000, "gateways": [] }
     }
   }

Common fields:

* ``enabled`` — Host calls ``init`` / ``start`` only when true
* ``devices`` — bound-device list; fields depend on the protocol (below)

Fields the Host reads per adapter:

* **matter** — ``devices[]`` (max 16). Commissioning: :doc:`matter`. Fields:

  * ``point_id`` (required) — max 64 bytes
  * ``type`` — max 16 bytes, default ``light``
  * ``node_id`` — fabric node id as decimal or ``0x`` hex string. The
    commissioner uses 1; end devices should be ≥ 2
  * ``endpoint_id`` — Matter endpoint, integer 0–65535, default 1 (this is
    what Host reads; REST upsert writes this field, not an ``endpoints`` array)
  * ``transport`` — ``wifi`` / ``ethernet`` / ``thread``, max 15 bytes; stored
    for display only
* **zigbee** — ``devices[]`` (max 16). Each entry registers one point:

  * ``point_id`` (required) — Hub id, max 64 bytes, two-segment dotted form
    e.g. ``zigbee.zb_0a1b`` (discovery) or ``zigbee.door_contact`` (manual bind)
  * ``type`` (required) — max 16 bytes. ``light`` / ``switch`` take on/off
    commands (``turn_on`` / ``turn_off`` / ``toggle``). ``sensor`` is
    temperature/humidity or contact
  * ``ieee`` — 64-bit IEEE as hex (optional ``0x`` prefix); used to recognize
    the device after join
  * ``short_addr`` — network short address, integer 0–65535
  * ``endpoint`` — Zigbee endpoint, integer 0–255, default 1
  * ``kind`` — only when ``type`` is ``sensor``: ``contact`` maps IAS Zone
    (point value ``open`` / ``closed``)
  * ``caps`` — only when ``type`` is ``sensor`` and ``kind`` is omitted:
    integer capability bitmask. If omitted, defaults to temp+humidity (bit1 |
    bit2). Other bits: bit0 on/off, bit3 IAS Zone, bit4 electrical measurement,
    bit5 energy metering. Non-``sensor`` types always get on/off (bit0);
    ``kind`` / ``caps`` are ignored
* **ble_mesh** — Provisioning policy lives under ``provision`` (``uuid_filters`` /
  ``bind_models`` may also sit at the adapter-block top level for L1 inject).
  ``devices[]`` max 16.

  * ``provision.uuid_match_disabled`` — true accepts any unprovisioned UUID
  * ``provision.uuid_filters[]`` — max 8. ``prefix_hex`` (or ``hex``) is a UUID
    prefix, up to 16 bytes; ``offset`` is 0–15 and offset+length must be ≤ 16.
    No valid filter means accept all UUIDs
  * ``provision.bind_models[]`` — max 12. ``model_id`` is an integer or
    ``0x1000``-style string; optional ``name``, ``company_id`` (SIG default
    ``0xFFFF``). Host clients today cover OnOff ``0x1000``, Level ``0x1002``,
    Lightness ``0x1300``, CTL ``0x1303``
  * ``devices[]``: ``point_id``, ``type`` (required, max 64 / 16 bytes).
    ``type`` ``sensor`` entries are skipped. ``uuid`` is 32 hex digits
    (32+NUL buffer). ``unicast`` 0–65535. ``elem_num`` 0–255, default 1
* **thread** — no ``devices`` and **no** Hub points (Border Router / dataset
  only; Matter-over-Thread end-devices live under ``adapters.matter``). Fields:

  * ``dataset`` — operational-dataset TLVs as hex (even number of digits). Empty
    or ``null`` makes Host form a new network and write the hex back. Also
    ``GET`` / ``POST /api/thread/dataset``. Binary size is limited by the
    OpenThread buffer (~254 bytes)
  * ``channel`` — status written back (current channel); not required to start.
    See :doc:`thread`
* **mqtt** — connect a broker and register points. ``devices[]`` max 16.

  * ``broker`` — hostname or IP, max 63 bytes. If unset, points still register
    offline
  * ``port`` — broker port, default 1883
  * ``devices[]``: ``point_id``, ``type`` (required). ``state_topic`` /
    ``command_topic`` max 95 bytes. Commands ``turn_on`` / ``turn_off`` /
    ``toggle`` publish ``ON`` / ``OFF`` on ``command_topic``; ``state_topic``
    payloads map to point ``on`` / ``off``
* **modbus** — master polling. Max 8 gateways and 64 maps total.

  * ``poll_ms`` — poll period, integer ≥ 500, default 2000
  * ``gateways[]``:

    * ``id`` — max 31 bytes, default ``gw0``, ``gw1``, …
    * ``transport`` — ``tcp`` (default) / ``rtu`` (aliases ``rs485`` /
      ``serial``) / ``rtuovertcp`` (alias ``gateway``)
    * ``host`` — required for TCP / rtuovertcp, max 63 bytes
    * ``port`` — TCP port 1–65535, default 502
    * ``timeout_ms`` — default 1000
    * ``unit_id`` — default slave id 0–255, default 1
    * RTU also: ``uart_port``, ``baudrate`` (default 9600), ``parity``
      (``none`` / ``even`` / ``odd``), ``data_bits`` 5–8 (default 8),
      ``stop_bits`` 1 or 2, ``tx_pin`` / ``rx_pin`` / ``rts_pin`` (default -1)
  * ``maps[]`` (under each gateway) map registers to points:

    * ``point_id`` (required) — max 64 bytes
    * ``type`` — max 16 bytes, default ``sensor``
    * ``unit_id`` — overrides the gateway default slave id
    * ``fc`` — ``coil`` / ``discrete`` (or ``di``) / ``input`` (or ``ir``);
      anything else is holding
    * ``address`` — 0-based PDU address, 0–65535
    * ``count`` — default 1
    * ``datatype`` — ``u16`` (default) / ``s16`` / ``u32`` / ``s32`` / ``f32``
    * ``scale`` / ``offset`` — default 1.0 / 0.0
    * ``writable`` — true allows writes
    * ``batch_as_props`` — true stores a register batch in point ``props``

Store modules (L2/L3) add their own key, for example ``my_module``. Install creates
the slot if missing. L1 packs **merge** ``inject`` into the named adapter block
(see :doc:`plugins/develop_l1`).

Do **not** put vendor cloud tokens in the generic Host factory JSON. Persist them via
the module (NVS / module-private files) after the user logs in.

Rules (``rules.json``)
----------------------

Full ``when`` / ``if`` / ``then`` tables, comparison ops (including ``ne``),
and the four action types: :doc:`rules`. Import:
``POST /api/rules/import``. MCP: ``rules.list`` / ``rules.put``.

Scenes (``scenes.json``)
------------------------

.. code-block:: json

   {
     "scenes": [
       {
         "id": "all_off",
         "name": "All off",
         "commands": [
           { "point_id": "sim.color_bulb", "cmd": "turn_off", "delay_ms": 0 }
         ]
       }
     ]
   }

Invoke: ``POST /api/scenes/invoke`` with ``{"scene_id":"all_off"}``.

Events
------

Adapters and JS/native modules publish on the Hub event bus. The Web UI
subscribes at ``GET /ws/events``. Rules listen to ``point.updated``. Event
type names: :doc:`api/host_rest` (``GET /ws/events``). You do not register
WebSocket handlers in a plugin — publish points/commands and the Hub forwards
events.

Related
-------

* Type catalog and UI: :doc:`type_catalog`
* Rules engine: :doc:`rules`
* REST examples: :doc:`api/host_rest`
* End-to-end flow: :doc:`architecture_overview`
* Matter commissioning REST: :doc:`matter`
* L1 inject merge: :doc:`plugins/develop_l1`
* L3 plugins: :doc:`plugins/develop_l3`

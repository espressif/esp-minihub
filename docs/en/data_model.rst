Data model
==========

:link_to_translation:`zh_CN:[中文]`

The **runtime point model** lives in ``adapters.json`` and scenes. Rules have
their own chapter (:doc:`rules`); device types, widgets and icons for the
Overview page are in :doc:`type_catalog`.

What the model looks like
-------------------------

Core does not know what a lamp is. It works on **points**: a type is a name, and
the parameters hang off the point as JSON, so a new device type needs no Core
change.

* **One physical device = one** ``minihub_point_t`` (Matter is **one
  endpoint** — see :doc:`matter`)
* ``point_id`` = ``protocol.device`` (e.g. ``sim.js_kitchen``,
  ``zigbee.zb_7d4b``, ``thread.matter_n2_ep1``)
* Live parameters live on that point under ``props.params.<name>``
* ``type`` is a string (``light`` / ``switch`` / …) — **not** a closed Core enum
* **How the UI draws** comes from the Host **type catalog** (CSV),
  protocol-agnostic — see :doc:`type_catalog`. The catalog is data, but its
  built-in copy is compiled into the firmware: editing the CSVs under
  ``components/minihub_device_widget/model/`` means a rebuild; editing
  ``/data/config/devices.csv`` on the device does not.

.. figure:: ../_static/data_model/point_lifecycle.en.svg
   :alt: State rises from the device through its adapter into props.params; the user reads it in the Overview page, rules and scenes, and commands take the same path back to the device
   :width: 100%

   Red dots show state rising from the device into the point and on to the user; the
   amber dashed path carries commands back down to the device. The Overview page,
   rules and scenes all read the same ``props.params``.

The UI shows a parameter only when the type catalog has it **and** the live
point carries it under ``props.params``.

Do **not** split one device into many points (wrong: ``sim.js_kitchen.brightness``).

Live point example (``GET /api/points``):

.. code-block:: json

   {
     "point_id": "sim.js_kitchen",
     "type": "light",
     "adapter": "color_bulb_js",
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
``owner``         After a plugin associates the device, Overview moves
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
``color_bulb_js`` / ``meter_plug_js`` set
``manufacturer=Espressif`` and a product ``category`` on register.

``minihub_point_set`` publishes ``point.updated`` (WebSocket ``GET /ws/events``)
only when the value string or the ``props`` JSON really differs — an identical
write is a silent no-op — and writes that land inside the point debounce window
are folded into one trailing publish (:doc:`api/host_rest`, ``GET /ws/events``).
A merge helper keeps the other top-level ``props`` keys; passing ``props`` to
``minihub_point_set`` replaces that object whole. Limits:
:doc:`api/minihub_core`.

Adapters that publish live parameters should call
``minihub_point_merge_params`` (or ``minihub_point_merge_props``) so other
top-level keys on the same point — ``ota``, ``ieee``, ``uuid``, ``node_id``, ``manufacturer``,
``category``, ``model``, ``sw_version``, ``hw_version``, ``owner``, … — are preserved. Plugins enrich
the **same** ``point_id``; they must not register a second point for the
same physical device.

Supported device types and icons
--------------------------------

The table below is generated at build time from
``components/minihub_device_widget/model/devices.csv``, so it lists exactly the types
the firmware ships. An icon comes from the plugin's own ``props.icon`` when it sets
one, else from the catalog ``image`` (an SVG under ``/img/devices/``), else from
``/img/devices/<type>.svg``. If that file is missing, the console draws the glyph from
``TYPE_ICON`` in ``app.js``, or the type's first letter. ``images/devices/`` also holds a
few icons used only by the Overview's dedicated panels (air conditioner, curtain,
television); those types are not in the catalog.

.. minihub-catalog:: devices

A ``type`` with no catalog row can still register a point and take part in rules. Its
detail panel draws rows for whatever parameters the instance reports; only the
type-level order, default bounds and avatar are missing.

Parameters and widget styles
----------------------------

The table is generated from ``parameters.csv``: one row per parameter, with its default
range, the widget the catalog picks for it, and the types that use it.

.. minihub-catalog:: parameters

.. figure:: ../_static/data_model/widget_styles.svg
   :alt: The parameter widget shapes: toggle, slider, dropdown, text, button and hidden
   :width: 100%

   Widgets are drawn by ``device_widgets.css``; parameter rows carry no icon. A
   read-only ``ui.text`` shows the value only.

Four things decide how a parameter row is drawn:

* **Shape** comes from the catalog ``ui_element``. The hue / saturation / CCT
  sliders get their spectrum track from a CSS tone picked by the parameter name; it is
  not a separate widget.
* **Value** comes from the live point's ``props.params``; a parameter that is not there
  gets no row.
* **Operability** follows the instance ``access``: ``R`` displays, ``RW`` allows change,
  and an unmet ``requires`` dims the control.
* **The power switch on a card** uses a separate hard-coded list (``ctrlTypes`` in
  ``overview.js``); types outside it get no switch, and the map popup toggles only
  ``switch`` and ``light``.

How parameters are read and written
-----------------------------------

``props.params`` has one writer: the adapter or plugin reporting state. Everything else
either reads it or sends a command.

.. list-table::
   :header-rows: 1
   :widths: 16 40 44

   * - Who
     - Reads
     - Writes
   * - Overview page
     - ``GET /api/device_widgets`` ∩ ``props.params``
     - a widget change → ``POST /api/devices/control``
   * - Rules
     - the ``props.params`` entry named by ``when.parameter``
     - the ``command`` action in ``then``
   * - Scenes
     - —
     - ``actions[].params``, passed to the adapter
   * - REST / MCP
     - ``GET /api/points``
     - ``POST /api/command``

After a command takes effect the adapter writes the new value back into
``props.params`` and publishes ``point.updated``, so the Overview page and the rules see
the same value. A plugin should not keep a second copy of that state.

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

* ``enabled`` — an adapter is skipped only when this is the JSON value
  ``false``; a missing or non-boolean ``enabled`` still loads and starts it.
  A radio-mutex conflict can still hold ``start`` back (see :doc:`radios`).
* ``devices`` — bound-device list; fields depend on the protocol (below)

Fields the Host reads per adapter:

* **matter** — ``devices[]``; the cap is ``CONFIG_MINIHUB_MATTER_MAX_NODES``
  (default 128, range 16–1024) and extra entries are dropped with an error log.
  This bounds the **configured** list; how many nodes the running commissioner can
  actually track and subscribe to is lower — see the limits in :doc:`matter`.
  Commissioning: :doc:`matter`. Fields:

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
  * ``channel`` — the factory seed carries this key, but no code reads or writes
    it: Host ignores it, and the live channel is reported by
    ``GET /api/thread/status``. See :doc:`thread`
* **mqtt** — connect a broker and register points. ``devices[]`` max 16.

  * ``broker`` — hostname or IP, max 63 bytes. If unset, points still register
    offline
  * ``port`` — broker port, default 1883
  * ``devices[]``: ``point_id``, ``type`` (required). ``state_topic`` /
    ``command_topic`` max 95 bytes. Commands ``turn_on`` / ``turn_off`` /
    ``toggle`` publish ``ON`` / ``OFF`` on ``command_topic``. A ``state_topic``
    payload is read as 31 bytes: ``ON*`` / ``OFF*`` in any case become ``on`` /
    ``off``, ``pressed`` / ``released`` pass through as those values, and
    anything else becomes the point value verbatim
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
    * ``batch_as_props`` — with ``count`` > 1, also store the raw register batch
      in the point's ``props`` as ``r0`` … ``r31`` (first 32 registers only)

Store modules (L2) add their own key, for example ``my_module``. Install creates
the slot if missing. L1 packs **merge** ``inject`` into the named adapter block
(see :doc:`plugins/develop_l1`).

.. only:: not hide_l3

   L3 modules add their own adapter key the same way.

Do **not** put vendor cloud tokens in the generic Host factory JSON. Persist them via
the module (NVS / module-private files) after the user logs in.

Rules (``rules.json``)
----------------------

Full ``when`` / ``if`` / ``then`` tables, comparison ops (including ``ne``), and
the action types (``command``, ``command_by_type``, ``notify``, plus the legacy
``delay``): :doc:`rules`. Import: ``POST /api/rules/import``. MCP:
``rules.list`` / ``rules.put``.

A rule reads parameters, not the UI. ``when.parameter`` names a parameter (for example
``brightness``) and the comparison uses that entry's ``value`` in ``props.params``;
without ``parameter`` the rule compares the point summary ``value``. Either way a rule
fires on a **change** into the matching state, not while the value sits there. A
``command`` action in ``then`` changes state, so a rule goes through the adapter too
and never writes ``props.params`` directly.

Scenes (``scenes.json``)
------------------------

A scene is a list of **steps**; a step may carry several commands and may repeat, which
makes it a one-tap sequence across devices. The root object holds a ``scenes`` array:

.. code-block:: json

   {
     "scenes": [
       {
         "id": "good_night",
         "name": "Good night",
         "steps": [
           {
             "actions": [
               { "point_id": "sim.js_kitchen", "cmd": "turn_off" },
               { "point_id": "sim.js_desk", "cmd": "set_brightness",
                 "params": { "parameter": "brightness", "value": 40 } }
             ],
             "loop": 1,
             "delay_after_ms": 0
           },
           {
             "actions": [ { "point_id": "sim.plug", "cmd": "turn_off" } ],
             "loop": 3,
             "delay_after_ms": 1000
           }
         ],
         "max_duration_ms": 0
       }
     ]
   }

Fields:

* ``id`` — scene identifier, matched exactly on invoke. The Web editor generates
  it from the name plus a timestamp and keeps it out of the edit form, but it
  does surface it as a fallback label when ``name`` is empty and in the delete
  confirmation. A scene without ``id`` loads and is listed, yet can never be
  invoked; with duplicate ids the first entry wins.
* ``name`` — display name. Only the Web editor requires it; the loader accepts a
  scene without one.
* ``steps[]`` — steps run **in order**:

  * ``actions[]`` — commands for this step, sent in array order (``cmd`` goes through
    ``minihub_command_invoke``, the same path as ``POST /api/command``). Each action
    needs a string ``point_id`` and a string ``cmd``, else that action is skipped;
    ``params`` is optional and only kept when it is a JSON object.
  * ``loop`` — how many times to repeat the step. Anything below 1, or not a
    number, becomes 1; there is no upper bound, so only ``max_duration_ms`` can
    stop a runaway repeat.
  * ``delay_after_ms`` — waited after **every** round, so a step with
    ``loop: 3`` and ``delay_after_ms: 1000`` sleeps 3 s in total, including once
    after its last action. Negative or absent becomes 0. Sleeps run in 200 ms
    chunks so a cancel is observed quickly.

* ``max_duration_ms`` — safety limit in milliseconds; only a positive number is
  kept, so ``0`` or absent means no limit. The deadline is checked before each
  action and before each 200 ms sleep chunk, and a breach stops the whole run
  immediately (not at the end of the step). It cannot interrupt an adapter
  command that is already running. The Web editor estimates the total
  (``Σ(loop × delay_after_ms)``) and refuses a limit **above** that estimate.

``params`` follows the command and the adapter. Devices-page widgets always send
``parameter`` plus ``value``; BLE Mesh ``set_brightness`` / ``set_color_temp`` read
``brightness`` / ``cct`` directly, and Modbus ``set`` reads only ``value``. When a
command takes effect the adapter writes ``props.params``; the scene itself never writes
a point.

Limits, applied silently by the loader: **8** scenes (``MAX_SCENES``), **5** steps
per scene (``MAX_SCENE_STEPS``), **8** actions per step (``MAX_STEP_ACTIONS``);
extra entries are dropped. Strings are truncated: ``id`` 31 characters, ``name``
63, ``point_id`` 63, ``cmd`` 31. The Web editor caps a step's delay at
**7200 s (7,200,000 ms)** but the loader does not — a longer value written into
``scenes.json`` by hand is kept, and ``max_duration_ms`` is what stops such a run.

**Backward compatibility:** the older flat ``commands:[{point_id,cmd,delay_ms}]``
form still loads when ``steps`` is absent. Each command becomes a one-action step
with ``loop=1`` and ``delay_after_ms=delay_ms``. That legacy path reads only those
three keys: a legacy ``params`` object is dropped by the firmware, although the
Web editor carries it and rewrites the scene into ``steps`` on save.

Invoke: ``POST /api/scenes/invoke`` with ``{"scene_id":"good_night"}`` (the key
``id`` is accepted too; 128-byte body). HTTP 200 means the run was **queued** on
the scene worker task, not that the actions finished. A new invoke, a config
reload, or a scene import asks the running scene to stop, and it notices between
actions or sleep chunks — so cancellation is not instant, and the next scene only
starts after the current one returns. Scenes are matched by ``id``, never by
``name``, and no scene progress is published on the event bus.

Events
------

Adapters and JS modules publish on the Hub event bus. The Web UI
subscribes at ``GET /ws/events``. Rules listen to ``point.updated``. Event
type names: :doc:`api/host_rest` (``GET /ws/events``). You do not register
WebSocket handlers in a plugin — publish points/commands and the Hub forwards
events.

Related
-------

* Type catalog and UI: :doc:`type_catalog`
* Rules engine: :doc:`rules`
* REST examples: :doc:`api/host_rest`
* End-to-end flow: :doc:`data_flow`
* Matter commissioning REST: :doc:`matter`
* L1 inject merge: :doc:`plugins/develop_l1`

.. only:: not hide_l3

   * L3 plugins: :doc:`plugins/develop_l3`

End-to-end data flow
====================

:link_to_translation:`zh_CN:[中文]`

.. contents::
   :local:
   :depth: 2

This page ties together the four subsystems you meet most when extending
MiniHub — the **device widget** (type catalog + UI), **Store modules**,
**rules**, and the **Web API** — and shows how a value travels from a
physical device to the browser and back. It complements :doc:`architecture`
(layers and principles), :doc:`type_catalog` (type catalog and UI), and
:doc:`data_model` (points and Hub config).

Big picture
-----------

MiniHub is a multi-protocol IoT hub firmware. One generic **Host** (Core +
bundled adapters) runs on the device; vendor/third-party behavior ships as
**Store modules** (``.mhub``), not as Host Core dependencies.

.. code-block:: text

   ┌──────────────────────────────────────────────────────────────┐
   │ Browser  ──  Web console (HTML/CSS/JS on modstore LittleFS)   │
   └───────────────┬───────────────────────────┬──────────────────┘
                   │ HTTPS (REST + WS)          │ WebSocket /ws/events
                   ▼                            │
   ┌──────────────────────────────────────────┼──────────────────┐
   │ minihub_web  (REST /api/*, /img/, /ws/events)                │
   ├──────────────────────────────────────────┼──────────────────┤
   │ minihub_hub  orchestration                                    │
   │  ├─ minihub_core      point registry  (one point per device)  │
   │  ├─ minihub_device_widget   type catalog (UI rendering)       │
   │  ├─ minihub_rules     when/if/then automation                 │
   │  ├─ minihub_scenes    scene recall                            │
   │  └─ minihub_store / minihub_module / minihub_pack  Store       │
   ├──────────────────────────────────────────────────────────────┤
   │ minihub_protocol_adapters  zigbee / matter / thread / ble_mesh / …     │
   │ examples/plugins + MINIHUB_PLUGIN_DIRS  Store .mhub sources    │
   └──────────────────────────────────────────────────────────────┘
                   │ protocol frames
                   ▼
            Physical devices / radios

Dependency direction is one-way downward: ``minihub_web`` depends on
``minihub_device_widget`` and ``minihub_core``; the device-widget component is
standalone and does **not** depend on Core at compile time. Adapters register
points into Core; nothing below calls back up into Web. (The on-device MCP
server, ``CONFIG_MINIHUB_MCP_SERVER``, is unrelated to the plugin-author IDE
MCP under ``tools/``.)

End-to-end data flow
--------------------

1. Discovery & registration
^^^^^^^^^^^^^^^^^^^^^^^^^^^

   An adapter (e.g. Zigbee, Matter, BLE Mesh) discovers a device and
   registers exactly **one** ``minihub_point_t`` per device (Matter: per
   endpoint). Point ids look like ``zigbee.zb_7d4b``,
   ``thread.matter_n2_ep1``, ``sim.js_kitchen``. The Thread adapter is a
   Border Router only and does **not** register device points — see
   :doc:`thread`. Live parameters, access, visibility, and instance bounds
   live in structured JSON on that point — typically
   ``props.params.<name> = { "value", optional access / visible / min / max /
   step / unit }``. The point also carries a ``type`` string (e.g. ``light``)
   that names the data-model type. See :doc:`data_model`; catalog and widgets:
   :doc:`type_catalog`.

2. Type catalog (device widget)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

   How a ``type`` renders is described **declaratively**, not in C. The
   authoritative source is ``components/minihub_device_widget/model/*.csv``
   (``devices.csv``, ``parameters.csv``, ``services.csv``, ``ui_elements.csv``).
   At build time ``components/minihub_device_widget/tools/gen_device_widget.py``
   folds those into a compiled-in registry (``MINIHUB_DW_EMBEDDED_CSV``) and
   the firmware flattens them to a runtime file ``/data/config/devices.csv``.
   At boot ``minihub_device_widget_init`` seeds that file (if missing) from the
   embedded copy, then loads it.

   The catalog is **protocol-agnostic**: it carries no transport field.
   Adapter prefixes (``zigbee.``, Matter ``thread.matter_n…`` or
   ``wifi.matter_n…``, and so on) come
   from point registration, so the same ``light`` / ``switch`` widgets render
   regardless of how the device arrived. Built-in types, widgets, and
   developer how-to: :doc:`type_catalog`.

3. Render
^^^^^^^^^

   The Devices page issues ``GET /api/device_widgets`` (handler
   ``handle_api_device_widgets`` in ``components/minihub_web/minihub_web.c``)
   plus ``GET /api/points``, then ``device_widgets.js`` intersects the type
   catalog with each live point's ``props.params`` and draws CSS widgets
   (``device_widgets.css``): ``toggle`` / ``slider`` / ``dropdown`` / ``text``
   / ``hidden``. One card is drawn per registered point (per device), never one
   card per parameter. Device avatars are served from ``GET /img/devices/…``.

4. Control
^^^^^^^^^^

   A tap on a widget calls ``POST /api/devices/control`` (handler
   ``handle_api_devices_control``) with JSON
   ``{ "device": type, "parameter": name, "value": …, "point_id": id, "seq": n }``.
   ``device`` is the **data-model type** (``light``, ``switch``, …).
   ``point_id`` is the live instance from the card (e.g. ``sim.kitchen_lamp``);
   Host uses it when the point exists and its type matches, otherwise falls
   back to first-match ``(type, parameter)`` resolve. The body is drained
   (max 1024 bytes); ``value`` is JSON-serialized so strings stay quoted.
   Numeric dropdown options stay numbers; non-numeric options (``cool`` /
   ``auto``) stay strings. Automation may also use ``POST /api/command`` with
   an explicit ``point_id`` (optional ``params.parameter``). L2 JS
   ``host.command.invoke`` is ``{ point_id, cmd, params? }``.

5. Events & rules
^^^^^^^^^^^^^^^^^

   Every point update emits a ``point.updated`` event. Two consumers:

   * The live UI refreshes over the WebSocket at ``/ws/events``
     (``components/minihub_web/minihub_web_ws.c``). Event types are listed
     under ``GET /ws/events`` in :doc:`api/host_rest`.
   * The rules engine (``minihub_rules``) evaluates ``when`` / ``if`` / ``then``.
     A match runs ``command``, ``command_by_type``, ``delay``, or ``notify``
     (see :doc:`rules`). Scenes are a separate ``POST /api/scenes/invoke``
     path, not a rule ``then`` type.

   Rules, scenes, and module RPCs are all REST-exposed (``/api/rules``,
   ``/api/scenes``, ``/api/modules/*``).

6. Store modules
^^^^^^^^^^^^^^^^

   Third-party behavior is delivered as **Store modules** under
   ``/store/modules/ID/``:

   * **L1** ``minihub_pack`` — injects into an existing adapter (no new radio
     stack).
   * **L2** ``minihub_module`` — a JavaScript plugin (``main.js``, QuickJS via
     ``minihub_js``); may ship ``web/`` served at ``/modules/ID/web/…``.

.. only:: not hide_l3

   * **L3** ``minihub_module`` — a native ``.so`` loaded at runtime.

The catalog of installable modules is the **Store** (``minihub_store``).
Demos live under ``examples/plugins/`` via ``MINIHUB_PLUGIN_DIRS``; the
public Host ``store_preinstall`` stays empty unless factory modules are
intentionally bundled. Modules subscribe to events, call Host APIs, and
register points — they never bake vendor tokens into Core.

Type-catalog runtime contract
-----------------------------

The catalog can be edited **without a firmware rebuild** via
``PUT /api/dev/catalog/{devices.csv|parameters.csv}`` (see
:doc:`api/host_rest`). The reload path is built to be failure-safe:

* **Atomic reload.** A new table is built fully before the in-memory registry
  is swapped. If the incoming file is unparseable, the previous table is kept
  and the request fails — the UI never reads a half-freed registry.
* **Bad rows are skipped, not fatal.** A single malformed row is logged and
  dropped; the remaining good rows still load (previously a bad row rejected
  the entire file and overwrote customizations).
* **Type-not-registered is visible.** When a point's ``type`` is empty or
  absent from the catalog, the card renders a "type not registered" badge and
  the console logs a warning, so a data problem is distinguished from a render
  problem.
* **Self-heal.** If ``/data/config/devices.csv`` is missing or corrupt at boot,
  the firmware re-seeds it from the embedded copy and reloads.

API surface quick reference
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Endpoint
     - Purpose
   * - ``GET /api/points``
     - List registered points (devices) and their ``props.params``
   * - ``GET /api/points/history``
     - Per-point value history
   * - ``POST /api/points/remove``
     - Forget a point
   * - ``GET /api/device_widgets``
     - Type catalog (labels / widgets); optional first-match ``point_id``
   * - ``POST /api/devices/control``
     - Devices page: ``device`` = type; optional ``point_id`` for the instance
   * - ``POST /api/command``
     - Command by ``point_id`` (optional ``params.parameter``)
   * - ``GET /api/adapters``
     - Adapter overview (enabled / module); not full ``adapters.json``
   * - ``GET|PUT /api/config/adapters``
     - Read / replace the adapters document
   * - ``GET|PUT /api/dev/catalog/{devices.csv|parameters.csv}``
     - Read / replace the type catalog at runtime
   * - ``GET /api/rules``, ``POST /api/rules/import``, ``POST /api/rules/ai/chat``
     - Rules management and AI helper
   * - ``GET /api/modules``, ``POST /api/modules``, ``POST /api/modules/*``
     - Store module list / install / RPC
   * - ``GET /api/modules/*/preview``
     - Module UI preview
   * - ``GET /api/packs``, ``POST /api/packs``
     - L1 pack list / manage
   * - ``GET /api/scenes``, ``POST /api/scenes/import``, ``POST /api/scenes/invoke``
     - Scenes management
   * - ``GET /api/events``, WebSocket ``/ws/events``
     - Point/event stream (REST poll + live socket)
   * - ``GET /img/`` plus subdir and file
     - Device / parameter icons (path-traversal guarded)
   * - ``POST /api/hub/reload``
     - Soft reload of Hub runtime (re-runs catalog reload)

Where to go next
----------------

* :doc:`type_catalog` — device types, widgets, and icons.
* :doc:`data_model` — the point model plus adapters / scenes.
* :doc:`rules` — ``when`` / ``if`` / ``then`` and comparison ops.
* :doc:`web_console` — replacing or skinning the Web console.
* :doc:`plugins/index` — authoring L1/L2 Store modules.
* :doc:`api/index` — full REST / JS / MCP / C ABI reference.

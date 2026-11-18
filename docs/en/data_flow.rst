End-to-end data flow
====================

:link_to_translation:`zh_CN:[中文]`

.. contents::
   :local:
   :depth: 2

A control action from the Web console, REST, or MCP forms a full loop: a command
goes down through an adapter to the device; the device reports its new state, the
value lands on the point and reaches the event bus as ``point.updated``; the bus
updates the UI and fires rules; a rule action can start the next round. When a
command fails to reach the device, the attempt is still on the bus as
``command.invoked``, but no ``point.updated`` follows and the loop stops at the
caller. Four subsystems take part: ``minihub_device_widget`` (the catalog), Store
modules, ``minihub_rules``, and the Web API. Each hands the value on without
knowing who will pick it up next.

.. image:: ../_static/data_flow/data_flow.svg
   :align: center
   :alt: MiniHub end-to-end data flow: control → device → point update → event bus → screen/rules → Store modules

`Interactive version <_static/data_flow/index.html>`_ — the same loop stages as the figure; each stage notes one design constraint (hover pauses).

For the static picture (which part may depend on which), read
:doc:`architecture`. For field definitions, read :doc:`data_model` and
:doc:`type_catalog`.

Dependency direction
--------------------

Dependencies point one way, downward. A change in a radio therefore does not
reach the UI through a compile-time dependency. ``minihub_web`` depends on
``minihub_device_widget`` and ``minihub_core``; the device-widget component is
standalone and does not depend on Core at compile time. Adapters register points
into Core, and nothing below Core calls back up into Web. The layering figure in
:doc:`architecture` shows the same stack from entry points down to silicon.

The on-device MCP server (``CONFIG_MINIHUB_MCP_SERVER``) is another way in: it
reads the same point registry and calls the same command router on demand, so
its tools see exactly the points a browser sees. It does not subscribe to the
event bus, and it is unrelated to the plugin-author IDE MCP under ``tools/``.

One control round trip
----------------------

The stages below follow the same loop one hop at a time.

1. Control
^^^^^^^^^^

   A tap on a widget calls ``POST /api/devices/control`` (handler
   ``handle_api_devices_control`` in ``components/minihub_web/minihub_web.c``)
   with JSON
   ``{ "device": type, "parameter": name, "value": …, "point_id": id, "seq": n }``.
   ``device`` is the **data-model type** (``light``, ``switch``, …). Host uses
   ``point_id`` when the point exists, its type matches, **and** it carries the
   named parameter; otherwise it falls back to first-match ``(type, parameter)``
   resolve, and a total miss answers **404**. The body is drained into a fixed
   1 KiB buffer, so the largest body that is accepted is 1023 bytes; ``value`` is
   JSON-serialized so strings stay quoted, and one that will not fit the 256-byte
   value buffer is refused. A boolean becomes the ``toggle`` command, anything
   else becomes ``set``. ``seq`` is a client timestamp (``Date.now()``, kept to 32
   bits) that comes back inside the response ``payload`` — it never appears on the
   bus. The response is ``{ "result", "path", "parameter", "payload" }``; failures
   are **401** (no session, checked before the body is read), **400** (missing,
   malformed or oversized body, or a value too long to serialize), **404** (no such
   point), or **500** (adapter invoke failed, or out of memory). There is no
   "busy" code on this path.

   Every command source converges on the same router
   (``minihub_command_invoke``): the widget POST above, ``POST /api/command``
   (explicit ``point_id``, optional ``params.parameter``), rule ``command`` /
   ``command_by_type`` actions, and JS ``host.command.invoke`` /
   ``host.command.on``. The adapter for the resolved point then delivers the
   command over the radio.

2. Device acts and reports
^^^^^^^^^^^^^^^^^^^^^^^^^^

   The adapter delivers the command over its radio (Zigbee, Matter, BLE Mesh,
   and so on); the device acts and reports its new state back to the adapter.
   Protocol details: :doc:`zigbee`, :doc:`matter`, :doc:`ble_mesh`. When
   delivery fails, the adapter returns the error and the router logs a warning.
   ``command.invoked`` has already gone out by then (step 4), so the attempt is
   visible on the bus, but no ``point.updated`` follows: the loop does not close,
   and the caller learns of the failure from its HTTP status alone.

3. Point update
^^^^^^^^^^^^^^^

   The adapter writes the new value back into the registered point
   (``minihub_point_t``) as structured JSON, typically
   ``props.params.<name>.value``; live parameters, access, visibility, and
   instance bounds all live on that same point. Point ids look like
   ``zigbee.zb_7d4b``, ``thread.matter_n2_ep1``, ``sim.js_kitchen``; the point
   also carries a ``type`` string (e.g. ``light``) that names the data-model
   type. See :doc:`data_model`.

4. Event bus
^^^^^^^^^^^^

   A **changed** value or changed ``props`` emit ``point.updated``; re-writing the
   same value with the same props emits nothing. The first write of a burst
   publishes at once; later writes inside the debounce window
   (``CONFIG_MINIHUB_POINT_DEBOUNCE_MS``, default 1 s) update the point and leave
   one pending publish, which a periodic sweep emits after the window has run — a
   dragged slider therefore publishes its first and its final value, not every
   intermediate one. The event carries the point's registry
   ``point_index`` (not its id) plus ``old_value`` / ``new_value``.

   The bus is a real abstraction in ``minihub_core``: a subscription table
   (32 slots), a FreeRTOS queue (depth 32) drained by a single dispatch task,
   and an owner tag on every subscription, wildcard or not, so unloading a module
   cannot leave a dangling subscriber. Delivery is best-effort — a full queue drops
   events, and consumers must tolerate gaps. The same bus carries
   ``point.registered`` / ``point.unregistered``, ``command.invoked``
   (published *before* the adapter runs, on success or failure — but not when the
   point or its adapter handler cannot be resolved at all), ``hub.reloaded``,
   ``device_widgets.updated``, and ``store.module.installed``.

5. Consumers: screen and rules
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

   ``point.updated`` has several consumers, and none of them is special:

   * **The screen.** ``boot.js`` holds the WebSocket at ``/ws/events``
     (``components/minihub_web/minihub_web_ws.c``). The socket subscribes to
     **all** events and the browser filters by ``event_type``. A
     ``point.updated`` (or ``hub.reloaded``) schedules a debounced re-fetch of
     ``GET /api/points``; ``overview.js`` then redraws the grid — the loop
     closes here and the user sees the new value. ``device_widgets.js``
     supplies the catalog intersection and the per-parameter controls of the
     detail view. The socket table holds 8 clients; a 9th connection is not
     refused, it simply never receives a broadcast: the socket stays open while
     events stop. A dropped socket reconnects with backoff (2 s, ×1.5, capped at 15 s)
     and re-fetches, and a 3 s poll covers the gap while it is down.
   * **The rules engine** (``minihub_rules``) evaluates ``when`` / ``if`` /
     ``then`` per event — the debounce and no-op suppression above therefore
     also gate rules. A match runs ``command``, ``command_by_type``,
     ``delay``, or ``notify`` (see :doc:`rules`). A ``command`` action returns
     to step 1 and starts the next round; scenes are a separate
     ``POST /api/scenes/invoke`` path, not a rule ``then`` type.
   * **Store modules** call ``host.event.subscribe(type, fn)`` (step 6).

   Value history is recorded inline in the point write, not on the bus:
   value-change only, a 160-entry ring in PSRAM, at most 64 samples returned by
   ``GET /api/points/history``, and lost on reboot.

6. Store modules
^^^^^^^^^^^^^^^^

   Third-party behavior is delivered as **Store modules** under
   ``/store/modules/ID/``:

   * **JSON** ``minihub_pack`` — injects into an existing adapter (no new radio
     stack).
   * **JS** ``minihub_module`` — a JavaScript plugin (``main.js``, QuickJS via
     ``minihub_js``); may ship ``web/`` served at ``/modules/ID/web/…``.

.. only:: not hide_l3

   * **ELF** ``minihub_module`` — a native ``.so`` loaded at runtime.

Modules subscribe with ``host.event.subscribe(type, fn)`` (internally one
wildcard subscription, filtered in JS), call Host APIs, and register points —
module-registered points emit ``point.updated`` like any other, so the step-4
events reach them too. The catalog of installable modules is the **Store**
(``minihub_store``). Demos live under ``examples/plugins/`` via
``MINIHUB_PLUGIN_DIRS``; the public Host ``store_preinstall`` stays empty unless
factory modules are intentionally bundled. Modules never bake vendor tokens
into Core.

What the loop guarantees
------------------------

The loop is best-effort by design, and the guarantees are narrow:

* **A true no-op is invisible.** Re-writing the same value with the same props
  emits no event, so neither a redraw nor a rule evaluation happens. A props-only
  change does emit.
* **Bursts are coalesced.** The debounce window (default 1 s) means the screen
  and rules see the settled value, not every intermediate one.
* **Events can be dropped.** The bus queue is finite; under load a consumer
  may miss an event. The screen self-heals by re-fetching; rules only see what
  arrives.
* **Last writer wins.** Two clients (or a client and a rule) commanding the
  same point race, and nothing orders them. A point write is atomic under the
  registry mutex, but commands are not serialized: the router releases the adapter
  lifecycle lock before calling the handler, so a command doing LAN or cloud I/O
  cannot stall ``GET /api/adapters``. That lock covers the handler lookup and the
  reload pin, not the device call itself.
* **History is volatile.** ``GET /api/points/history`` reads RAM only and is
  cleared on reboot.

One-time setup at boot
----------------------

The round trip above relies on two things prepared once at boot: registered
points and renderable cards.

1. Discovery & registration
^^^^^^^^^^^^^^^^^^^^^^^^^^^

   An adapter (e.g. Zigbee, Matter, BLE Mesh) discovers a device and
   registers **one** ``minihub_point_t`` per device — per endpoint for Matter, per
   register-map entry for Modbus, so one device can legitimately hold several
   points. Registration is
   idempotent by id — an existing point is updated, not duplicated. The Thread
   adapter is a Border Router only and does **not** register device points —
   see :doc:`thread`. Without this step, the step-1 ``point_id`` has nothing to
   resolve to.

2. Type catalog and render
^^^^^^^^^^^^^^^^^^^^^^^^^^

   How a ``type`` renders is described **declaratively**, not in C. Only two of
   the four model CSVs drive the runtime registry: ``devices.csv`` (type →
   parameters, primary control, image) and ``parameters.csv`` (per-parameter
   schema: widget, options, access, unit). ``services.csv`` is reference
   material (RainMaker system services) and never enters the registry;
   ``ui_elements.csv`` is the closed widget set that the ``ui_element`` column
   of ``parameters.csv`` is validated against. At build time
   ``components/minihub_device_widget/tools/gen_device_widget.py`` folds
   ``devices.csv`` and ``parameters.csv`` into one flat registry text and compiles
   it in as ``MINIHUB_DW_EMBEDDED_CSV``. The flattening happens there, not in the
   firmware: the boot-time seed writes that text verbatim to
   ``/data/config/devices.csv``, and only an uploaded model CSV is flattened at
   runtime. At boot ``minihub_device_widget_init``
   seeds that file (if missing or corrupt) from the embedded copy, then loads it.

   The catalog is **protocol-agnostic**: it carries no transport field.
   Adapter prefixes (``zigbee.``, Matter ``thread.matter_n…`` or
   ``wifi.matter_n…``, and so on) come from point registration, so the same
   ``light`` / ``switch`` widgets render regardless of how the device arrived.
   Built-in types, widgets, and developer how-to: :doc:`type_catalog`.

   The Overview page issues ``GET /api/device_widgets`` (handler
   ``handle_api_device_widgets`` in ``components/minihub_web/minihub_web.c``)
   plus ``GET /api/points``; ``device_widgets.js`` intersects the catalog with
   each live point's ``props.params`` and ``overview.js`` draws the grid. The
   widget kinds are the closed ``ui_element`` set in
   ``components/minihub_device_widget/model/ui_elements.csv``, listed with their
   rendering in :doc:`type_catalog`. One card is drawn per registered
   point, never one card per parameter; the detail view exposes
   the per-parameter controls. Device avatars are served from
   ``GET /img/devices/…``. Without this step, the step-1 cards do not exist.

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
* **An unregistered type leaves a log line.** When a point's ``type`` is empty or
  absent from the catalog, the detail panel still draws its rows and the browser
  console logs ``type not in catalog``, so a data problem is distinguished from a
  render problem.
* **Self-heal.** If ``/data/config/devices.csv`` is missing or corrupt at boot,
  the firmware re-seeds it from the embedded copy and reloads.
* **Two files, one model.** A ``PUT`` of only one of the two model CSVs answers
  OK with a hint and does **not** rebuild the registry until its partner
  arrives; ``DELETE /api/dev/catalog/*`` drops the runtime copy and falls back
  to the embedded one.

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
   * - ``POST /api/points/rename``
     - Rename a point's display name
   * - ``GET /api/device_widgets``
     - Type catalog (labels / widgets); optional first-match ``point_id``
   * - ``POST /api/devices/control``
     - Widget control: ``device`` = type; optional ``point_id`` for the instance
   * - ``POST /api/command``
     - Command by ``point_id`` (optional ``params.parameter``)
   * - ``GET /api/adapters``
     - Adapter overview (enabled / module); not full ``adapters.json``
   * - ``GET|PUT /api/config/adapters``
     - Read / replace the adapters document
   * - ``GET|PUT /api/dev/catalog/{devices.csv|parameters.csv}``
     - Read / replace the type catalog at runtime
   * - ``DELETE /api/dev/catalog/{devices.csv|parameters.csv}``
     - Drop the runtime copy; falls back to the embedded one
   * - ``GET /api/rules``, ``POST /api/rules/import``, ``POST /api/rules/ai/chat``
     - Rules management and AI helper
   * - ``GET /api/modules``, ``POST /api/modules``, ``POST /api/modules/*``
     - Store module list / install / RPC
   * - ``GET /api/modules/*/preview``
     - Module UI preview
   * - ``GET /api/packs``, ``POST /api/packs``
     - JSON pack list / manage
   * - ``GET /api/scenes``, ``POST /api/scenes/import``, ``POST /api/scenes/invoke``
     - Scenes management
   * - ``GET /api/events``, WebSocket ``/ws/events``
     - Recent event ring (REST poll) plus the live socket
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
* :doc:`plugins/index` — authoring JSON/JS Store modules.
* :doc:`api/index` — full REST / JS / MCP / C ABI reference.

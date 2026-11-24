Develop JS (JavaScript) Plugins
===============================

:link_to_translation:`zh_CN:[中文]`

JS modules run **QuickJS** scripts on the Hub. Host loads ``main.js`` after
adapters start and on soft reload (``POST /api/hub/reload``). Implementation:
``components/minihub_js/minihub_js.c``.

Runtime
-------

The engine is QuickJS-NG, the community fork of Bellard's QuickJS, and this
firmware pins the ``espressif/quickjs-ng`` component at 0.14.0. Syntax and
capability limits follow the ``quickjs-ng`` docs and the linked build:
``class``, ``async``/``await``, ``Proxy`` and optional chaining all work.
MiniHub adds four global timers and one ``host`` object, and no other runtime
plugins, so a JS module is neither a browser page nor a Node process.

Differences from browser and Node runtimes:

* One file, no module system. The Host runs each ``main.js`` inside its own
  function (``minihub_js.c``), so a top-level ``function`` or ``var`` belongs to
  that module and does not replace the same name in another module. ``import``
  and ``require`` still resolve to nothing, and there is no file-read Host API
  either — each module is a single ``main.js``.
* No ``console`` and no ``print``. Logging goes to ``host.log.info`` /
  ``host.log.warn`` / ``host.log.error``.
* No ``fetch``, no ``XMLHttpRequest``, no file system. Outbound HTTP is
  ``host.http.request`` / ``host.http.requestAsync``; persisted state is
  ``host.config``. See :doc:`../api/js_host`.
* ``setTimeout`` / ``setInterval`` / ``clearTimeout`` / ``clearInterval`` are
  installed as globals and repeated under ``host.timer``. Every callback runs on
  one JS worker task, so a callback that blocks holds up all other JS modules.
* The worker task stack is ``CONFIG_MINIHUB_JS_WORKER_STACK_SIZE`` (32768 bytes
  by default, allocated in PSRAM) and QuickJS is told to stop at
  ``CONFIG_MINIHUB_JS_WORKER_STACK_BUDGET`` (24576) so recursion hits the engine
  limit instead of the real stack.
* JS needs ``CONFIG_MINIHUB_JS_PLUGINS``, which defaults to on once
  ``CONFIG_MINIHUB_DYNAMIC_MODULES`` is on.

When to use JS
---------------

* Lightweight automation, HTTP polling, or glue between REST and points
* UI snippets under ``/modules/`` without native code

.. only:: not hide_l3

   * Prototypes before committing to ELF

Requirements: ``CONFIG_MINIHUB_DYNAMIC_MODULES`` and JS runtime enabled in the
product sdkconfig (``dynamic_modules`` feature in ``app.yml``).

Directory layout
----------------

.. code-block:: text

   examples/plugins/ID/
     module.json
     main.js              # required — defines setup(host); without it the
                          # module still loads, with a no setup(host) function
                          # warning and no points
     meta.json            # optional ui + capabilities
     web/                 # optional static assets

``module.json`` example
-----------------------

.. code-block:: JSON

   {
     "id": "demo_js",
     "name": "Demo JS module",
     "version": "0.1.0",
     "enabled": true,
     "type": "module",
     "runtime": "js",
     "adapter": "demo_js",
     "targets": ["any"],
     "requires": []
   }

**Always set ``requires`` deliberately.** It is copied into ``catalog.json`` and
is what ``caps=`` on ``GET /api/catalog`` filters against. Tokens come from
``Zigbee``, ``BLE_mesh``, ``Thread_Matter``, ``Matter``, ``MQTT_Broker``, and
``Modbus``. ``[]`` means the module needs none of those stacks. While any listed
token is off or not built into the Hub, the store hides a not-yet-installed
module; an already installed module is not started on the next boot and is
labelled as not supported by this firmware. Do not confuse this with
``meta.json`` ``capabilities`` (``points``, ``auth``, …), which never gate the
catalog.

A module that uses the on-device MQTT broker lists that capability explicitly:

.. code-block:: JSON

   {
     "id": "esphome",
     "name": "ESPHome MQTT discovery",
     "version": "0.1.0",
     "runtime": "js",
     "adapter": "esphome",
     "targets": ["any"],
     "requires": ["MQTT_Broker"]
   }

``targets`` uses ``any`` for every chip; ``ALL`` is accepted and stored as
``any``.

A JS manifest carries no ``abi_version`` — that belongs to a native package.
``ui`` and ``capabilities`` are read from ``meta.json`` when one is packaged, and
fall back to ``module.json`` when it is not.

**``runtime: "js"`` is required.** Omitting it does not make the module JS: at
load time the runtime only starts a QuickJS module when ``runtime`` is the literal
string ``JS`` (or ``hybrid``), and a manifest with no ``runtime`` and no
``binary`` falls through to the native path and fails with "no binary or builtin".
The ``main.js`` / no-``CMakeLists.txt`` heuristic you may have seen described
elsewhere is used by the **packer and catalog generator** to classify a source
tree, not by the Hub. ``adapter`` is required too — the loader rejects a manifest
whose adapter name is empty, JS included.

``main.js`` contract
--------------------

Host calls ``setup(host)`` once per load. The ``host`` object exposes:

* ``host.log.info/warn/error(msg)``
* ``host.point.register({ point_id, type, adapter, value, online, props|params })`` —
  one device one point (``protocol.device``). ``type`` must be a catalog
  ``device_id`` (e.g. ``light`` / ``switch``) for standard Devices-page
  widgets; see :doc:`../type_catalog`. Pass ``params`` (shorthand) or full
  ``props`` so ``props.params`` reaches the UI.
* ``host.point.get(point_id)`` · ``host.point.set(point_id, value, props?)`` ·
  ``host.point.unregister(point_id)``
* ``host.event.subscribe(event_type, handler)`` — ``point.updated`` /
  ``point.registered`` (same Host ``point_id`` enrichment)
* ``host.command.on(adapter, handler)`` — receive UI/REST commands in JS
* ``host.command.invoke({ point_id, cmd, params? })``
* ``host.adapter.poll(fn)`` — run ``fn`` on the ~5 s adapter poll tick, up to 8
  handlers in the runtime; ``host.adapter.clearPoll()`` drops them all
* ``host.http.request({ url, method, headers, body, timeout_ms, response_b64 })``
  — blocking HTTP **on the JS worker**, so it stalls every other JS module for the
  duration. There is no URL allowlist in the device: reachability, and the TLS
  trust bundle for ``https``, are the only limits. ``response_b64`` returns the
  body base64-encoded instead of as a UTF-8 string
* ``host.http.requestAsync(opts)`` — same ``opts``, returns a **Promise** and runs the
  socket I/O off the JS worker (max 4 in flight). Use it for many-request paths such as
  bulk cloud refresh; there is no synchronous fallback for older Hosts
* ``host.config.get()`` — read merged ``adapters.json`` snippet

Plugins that enrich a Zigbee (or other Host) device must use the
**same** ``point_id``. Write extras under top-level ``props``:
``manufacturer`` / ``category`` / ``ota`` / ``owner`` are identity and
grouping metadata — not catalog params. Adapter telemetry merges
``props.params`` only, so those extras survive. Plugins that own the point
should set manufacturer and category on ``register`` (demo
``color_bulb_js``). Full Host API: :doc:`../api/js_host`.

Devices-page demo (three lights): ``examples/plugins/color_bulb_js/``.

Minimal ``main.js``:

.. code-block:: javascript

   function setup(host) {
     host.log.info("demo_js starting");
     host.point.register({
       point_id: "demo_js.hello",
       type: "sensor",
       adapter: "demo_js",
       value: "ok",
       online: true
     });
   }

``sensor`` in that sample is what the loader fills in when ``type`` is omitted
(``minihub_js.c``), and it is **not** a catalog ``device_id``: the point still
registers, works in rules and answers REST, but the console draws no widget card
for it — it shows the *type not registered* fallback, rendering each parameter
that the parameter table knows and a read-only text row for the rest
(:doc:`../type_catalog`). Name a real ``device_id`` (``light``, ``switch``,
``sensor_contact``, …) to get the card.

Optional ``meta.json`` describes module fields in the Web UI (see
``examples/plugins/color_bulb_js/`` for a Devices-page demo).

Wire into ``adapters.json``
---------------------------

Add a block so the adapter name exists (can be empty devices list):

.. code-block:: JSON

   {
     "adapters": {
       "demo_js": { "enabled": true, "devices": [] }
     }
   }

Pack, deploy, test
------------------

.. code-block:: bash

   python3 tools/pack_store_module.py pack demo_js
   python3 tools/sync_store_catalog.py --from-store
   python3 tools/store_server.py

Then in the Web UI at ``http://HUB/``, open ``Store``, set the custom Store URL
to ``http://YOUR_PC:8080``, refresh the catalog, and install ``demo_js``. The
Hub reloads the module automatically.

Verify:

* ``GET /api/modules`` — ``demo_js`` listed, ``loaded: true``
* ``GET /api/points`` — ``demo_js.hello`` present
* Serial log: ``JS module 'demo_js' setup ok``

Module RPC from JS is optional; prefer ``host.command`` or REST. JS API: :doc:`../api/js_host`.

.. only:: not hide_l3

   For ELF RPC see :doc:`develop_elf`.

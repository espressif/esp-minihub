Develop L2 (JS) Modules
=======================

:link_to_translation:`zh_CN:[中文]`

L2 modules run **QuickJS** scripts on the Hub. Host loads ``main.js`` after
adapters start and on soft reload (``POST /api/hub/reload``). Implementation:
``components/minihub_js/minihub_js.c``.

When to use L2
---------------

* Lightweight automation, HTTP polling, or glue between REST and points
* UI snippets under ``/modules/`` without native code

.. only:: not hide_l3

   * Prototypes before committing to L3

Requirements: ``CONFIG_MINIHUB_DYNAMIC_MODULES`` and JS runtime enabled in the
product sdkconfig (``dynamic_modules`` feature in ``app.yml``).

Directory layout
----------------

.. code-block:: text

   examples/plugins/ID/
     module.json
     main.js              # required — must export setup(host)
     meta.json            # optional ui + capabilities
     web/                 # optional static assets

``module.json`` example
-----------------------

.. code-block:: json

   {
     "id": "demo_js",
     "name": "Demo JS module",
     "version": "0.1.0",
     "enabled": true,
     "runtime": "js",
     "adapter": "demo_js",
     "abi_version": 1,
     "core_api_min": 1,
     "core_api_max": 1,
     "capabilities": ["points"]
   }

If ``runtime`` is omitted, Host treats the module as JS when ``main.js`` exists
and there is no ``CMakeLists.txt``.

``main.js`` contract
--------------------

Host calls ``setup(host)`` once per load. The ``host`` object exposes:

* ``host.log.info/warn/error(msg)``
* ``host.point.register({ point_id, type, adapter, value, online, props|params })`` —
  one device one point (``protocol.device``). ``type`` must be a catalog
  ``device_id`` (e.g. ``light`` / ``switch``) for standard Devices-page
  widgets; see :doc:`../type_catalog`. Pass ``params`` (shorthand) or full
  ``props`` so ``props.params`` reaches the UI.
* ``host.point.get(point_id)`` · ``host.point.set(point_id, value, props?)``
* ``host.event.subscribe(event_type, handler)`` — ``point.updated`` /
  ``point.registered`` (same Host ``point_id`` enrichment)
* ``host.command.on(adapter, handler)`` — receive UI/REST commands in JS
* ``host.command.invoke({ point_id, cmd, params? })``
* ``host.http.request({ url, method, headers, body })`` — allowlisted HTTP from JS
* ``host.config.get()`` — read merged ``adapters.json`` snippet

Vendor plugins that enrich a Zigbee (or other Host) device must use the
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

Optional ``ui.json`` describes module fields in the Web UI (see
``examples/plugins/color_bulb_js/`` for a Devices-page demo).

Wire into ``adapters.json``
---------------------------

Add a block so the adapter name exists (can be empty devices list):

.. code-block:: json

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
   # on Hub: POST /api/store/config {"custom_url":"http://YOUR_PC:8080"}
   curl -X POST http://$HUB/api/store/refresh
   curl -X POST http://$HUB/api/store/install \
     -H 'Content-Type: application/json' \
     -d '{"type":"module","id":"demo_js"}'
   curl -X POST http://$HUB/api/hub/reload

Verify:

* ``GET /api/modules`` — ``demo_js`` listed, ``loaded: true``
* ``GET /api/points`` — ``demo_js.hello`` present
* Serial log: ``JS module 'demo_js' setup ok``

Module RPC from L2 is optional; prefer ``host.command`` or REST. JS API: :doc:`../api/js_host`.

.. only:: not hide_l3

   For L3 RPC see :doc:`develop_l3`.

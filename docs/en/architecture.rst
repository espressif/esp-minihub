Layers and principles
=====================

:link_to_translation:`zh_CN:[中文]`

Layering
--------

.. code-block:: text

   +------------------------------------------------------------------+
   |  Application                                                     |
   |  Web UI / REST / WS / MCP clients / Store catalog / CLI          |
   +------------------------------------------------------------------+
                                  |
   +------------------------------------------------------------------+
   |  MiniHub Host                                                    |
   |  minihub_web / minihub_mcp_*     Web console, REST, MCP          |
   |  minihub_core / minihub_hub      points, commands, rules, reload |
   |  minihub_module                  Store .mhub (L2 JS / L3 .so)    |
   +------------------------------------------------------------------+
                                  |
   +------------------------------------------------------------------+
   |  Device access                                                   |
   |  Static adapters (Kconfig)  Wi-Fi / Zigbee / BLE Mesh / Matter   |
   |                             Thread / MQTT / Modbus / ...         |
   |  Store plugins              vendor cloud / OEM; no Host fork     |
   |  Host-based product         less RAM; full IDF / MiniHub APIs    |
   +------------------------------------------------------------------+
                                  |
   +------------------------------------------------------------------+
   |  Field devices                                                   |
   |  lights, sensors, gateways, vendor-cloud endpoints               |
   +------------------------------------------------------------------+

   IP uplink: minihub_net — see :doc:`networking`.

Design principles
-----------------

**Core stays protocol-neutral.** Radio and IP stacks that Espressif owns
(Wi-Fi STA adapter, Zigbee, BLE Mesh, Matter, Thread, MQTT, Modbus) can link
statically into the Host. Vendor clouds and OEM differences stay out of Core — ship them as Store
plugins or a Host-based product. That keeps the generic Host rebuildable without
third-party credentials.

**Points are the only automation ABI.** One physical device is one point
(``protocol.device``). Parameters live in ``props.params``. Adapters hide
protocol details. A rule that says ``turn_on`` on ``sim.color_bulb`` still
works if you later replace that device with another adapter that registers the
same ``point_id``.

**Soft reload instead of reboot.** Changing ``adapters.json``, installing a
``.mhub``, or toggling a module calls ``minihub_hub_reload`` — adapters stop,
modules ``dlclose``, then load again. Matter+Thread may re-attach CHIP; wait
for Hub ready (MCP ``HUB_NOT_READY`` during reload).

**Two writable flash partitions.** The generic Host mounts ``storage`` at
``/data`` (Hub config) and ``modstore`` at ``/store`` (installed plugins and the
Web console). A full factory flash rewrites both; ``app-flash`` does not. See
:doc:`production`.

JSON authoring: :doc:`data_model` (points / adapters / rules); type catalog and
widgets: :doc:`type_catalog`. End-to-end flow (catalog, Devices page,
Store): :doc:`architecture_overview`.

Key components
--------------

* ``minihub_core`` — point registry, command router, event bus
* ``minihub_config`` — load/save ``/data/config/*.json``
* ``minihub_hub`` — soft reload orchestration
* ``minihub_web`` — HTTP REST, Web console, ``/ws/events``
* ``minihub_module`` — L2/L3 loader, ABI / host vtable
* ``minihub_pack`` — L1 config packs (``inject`` merge)
* ``minihub_store`` — catalog, install/upgrade/uninstall
* ``minihub_net`` — Ethernet / Wi-Fi STA
* ``minihub_matter`` — Matter controller glue
* ``minihub_js`` — QuickJS L2 runtime
* ``minihub_device_widget`` — type catalog (CSV → labels / widgets)
* ``adapters/*`` — static protocol stacks
* ``examples/plugins/ID/`` — in-tree Store demos (``MINIHUB_PLUGIN_DIRS`` for extras)

Storage layout (generic Host)
-----------------------------

Default writable root: ``/data`` (LittleFS on generic ``minihub`` product).

.. code-block:: text

   /data/config/
     adapters.json      Runtime adapter + device list (copied from factory JSON at first boot)
     rules.json         Automation rules
     scenes.json        Scene definitions
     devices.csv        Flattened type catalog
     model/             Optional uploaded devices.csv + parameters.csv
   /data/config/packs/  Optional L1 pack staging (product-dependent)
   /store/modules/ID/   Installed Store modules (module.json, .so, main.js, …)
   /store/preinstall/web/  Host Web console (index.html, login.html, …)
   /data/tmp/           MHMD extract / scratch (module loader)

NVS holds Wi-Fi credentials, Store URL overrides, OTA metadata — not the full
``adapters.json`` (that lives on LittleFS).

Boot sequence (``application/minihub``)
---------------------------------------

Typical order in ``main`` / bring-up:

1. Flash dispatcher, mount ``/data``, ``minihub_config_init()``
2. ``minihub_core_init()`` — point registry, event bus, command router
3. ``minihub_net_init()`` — Ethernet start; Wi-Fi **prepared only** (deferred STA)
4. Static Matter/Thread bootstrap as built (CHIP / OpenThread order in ``main``)
5. ``minihub_module_loader_init()``; load Store modules from ``/store/modules/``
6. ``minihub_web`` HTTP server (UI reachable while radios start)
7. ``minihub_adapter_load_all()`` + ``minihub_adapter_start_all()`` — Wi-Fi STA
   connect is the ``wifi`` adapter; radio mutex skips BLE Mesh if Wi-Fi +
   Zigbee/Thread are also enabled (see :doc:`radios`). MCP / ``MiniHub ready`` follow.

``wifi STA deferred`` in logs is expected until the ``wifi`` adapter starts.
See :doc:`networking`.

Hub soft reload
---------------

``POST /api/hub/reload`` (or MCP ``hub.reload``) runs ``minihub_hub_reload()``:

1. ``minihub_config_reload()`` — re-read JSON from flash
2. ``minihub_pack_reload()`` — merge L1 packs into adapters view
3. Stop all adapters; ``minihub_module_reload()`` — dlclose L3, rescan manifests
4. ``minihub_adapter_load_all()`` + ``start_all()``
5. ``minihub_js_reload()`` — re-run L2 ``setup(host)``
6. ``minihub_rules_reload()`` + ``minihub_scenes_reload()``

Adapter lifecycle lock prevents ``adapter_poll`` from running while a ``.so`` is
unloaded. Matter + Thread builds may re-attach CHIP to OpenThread after reload.

Data flow: command
------------------

.. code-block:: text

   REST POST /api/command
        → minihub_command_invoke
        → adapter.handle_command (static or Store module)
        → minihub_point_set / device I/O
        → minihub_event_bus_publish → WebSocket /ws/events

Data flow: Store install
------------------------

.. code-block:: text

   POST /api/store/install { kind, id }
        → download .mhub → unzip under /store/modules/ID/
        → optional hub reload
        → minihub_module_load_all → minihub_module_entry → register adapter

See :doc:`plugins/pack_and_catalog`, :doc:`api/host_rest`, :doc:`api/plugin_rest`.

Configuration sources
---------------------

* **Build time:** Kconfig, ``manifest.yml``, ``sdkconfig_features``
* **Factory JSON:** ``application/APP/config/adapters.json`` or repo ``config/``
* **Runtime:** Web UI, REST ``PUT /api/config/adapters``,
  ``PUT /api/dev/catalog/…``, module RPC that calls
  ``config_save_adapters`` via host vtable

Generated files (do not hand-edit)
----------------------------------

``sync_host_manifest.py`` writes:

* ``application/APP/main/idf_component.yml``
* ``application/APP/main/adapters_setup.generated.c``
* ``cmake/generated/minihub_components.cmake``

See :doc:`application/index` then :doc:`host_customization`.

Secondary development surface (public contract)
-----------------------------------------------

Supported extension surface for OEM Host and Store plugins.

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Surface
     - Chapter / artifact
   * - Product profile
     - ``manifest.yml``, ``partitions.csv``, ``config/sdkconfig/app.yml`` —
       :doc:`host_customization`
   * - Runtime JSON / catalog
     - ``adapters.json``, ``rules.json``, ``scenes.json`` —
       :doc:`data_model`; type catalog CSV — :doc:`type_catalog`
   * - HTTP / events
     - :doc:`api/host_rest`; WebSocket events at ``/ws/events``
   * - MCP tools
     - :doc:`api/mcp`
   * - L2 JS host object
     - :doc:`api/js_host`
   * - L3 module + adapter C API
     - :doc:`api/module_abi`, :doc:`api/minihub_core`, :doc:`api/minihub_adapter`
   * - L3 link symbols
     - ``docs/module_sdk/host_export_allowlist.json``
   * - Store pack / catalog
     - :doc:`plugins/pack_and_catalog`
   * - Factory / OTA
     - :doc:`production`

**ABI stability:** L3 ``.so`` modules declare ``abi_version`` and
``core_api_min`` / ``core_api_max`` in ``module.json``. Ship matched firmware +
``.mhub`` sets per release (see :doc:`production`). Host export changes require
allowlist and symbol regeneration (:doc:`plugins/develop_l3`).

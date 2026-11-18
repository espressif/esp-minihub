Layers and principles
=====================

:link_to_translation:`zh_CN:[中文]`

What runs on a Hub, which part is allowed to depend on which, and what happens
between power-on and the first device update. The end-to-end control loop is
in :doc:`data_flow`.

Layering
--------

.. image:: ../_static/architecture/layered.svg
   :align: center
   :alt: MiniHub bands from the user inward: entry points, Hub services, the two clouds, the type-agnostic core, runtime extension, the protocol side, silicon and build; a violet channel at the right edge marks the two upward extension mechanisms

A command enters through the console, REST or MCP, Host Core resolves it to one
point, and an adapter or Store module speaks the protocol; the resulting
``point.updated`` travels back the same path to the UI, the rule engine and the
MCP client. Directory names in a band are relative to the prefix on that band's
title row.

Two links run the other way. ``minihub_net`` calls
``minihub_web_refresh_geo_async()``, ``minihub_lcd_request_refresh()`` and
``minihub_rainmaker_notify_got_ip()`` as weak no-ops that it defines itself, so a
build linking those components replaces the stub without the network layer naming
them. And the Host entry points installed code may call are published as a vtable
in ``minihub_module``, not reached by depending on it.

A protocol is usually two directories — the stack and its adapter — and the two
are not one layer: the MQTT and Modbus adapters call only Core, while the Zigbee
and BLE Mesh ones also call their stack and the loader.

Two clouds, one installed and one linked in
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A Hub can end up talking to two kinds of cloud, and they differ in how the
speaking code got there.

* **A vendor cloud is installed.** MIoT and the like arrive as a Store module —
  a JSON pack merged into ``adapters.json``, or a JS module that signs in,
  subscribes and reports. It is installed, upgraded and removed against a running
  Hub, which is why vendor-specific code is never linked into the Host: uninstall
  the module and the path is gone.
* **RainMaker is linked in.** ``components/minihub_rainmaker`` behind
  ``CONFIG_MINIHUB_RAINMAKER`` (default off) compiles the ESP-IDF bridge into the
  image, so that cloud exists only on a build that enabled it, and changing it
  means a rebuild and an OTA.

What the adapters and modules reach besides clouds — child devices on Zigbee, BLE
Mesh and Matter-over-Thread, the MQTT broker and the deploy server a site runs —
is not a layer either, and each is described where it is implemented
(:doc:`radios`, :doc:`networking`, :doc:`production`).

Either way Host Core is not the one that speaks cloud. It owns points and the
event bus; the translation into a cloud's own device/service vocabulary happens
in the bundled bridge or in the installed module, so a cloud change never reaches
into Core.

IP uplink: ``minihub_net`` -- see :doc:`networking`.

`Interactive version <_static/architecture/index.html>`_ -- the same layering as
the static figure. Each band lists dependency rules; each block lists what it
must not call (hover pauses).

.. only:: not hide_l3

   ``minihub_module`` also loads ELF ``.so`` packages (and Hybrid).

Design principles
-----------------

Core does not know protocols. A radio or IP stack that works without a vendor
account or server (Wi-Fi STA adapter, Zigbee, BLE Mesh, Matter, Thread, MQTT,
Modbus) may link statically into the Host. Anything that needs an account or a
vendor server to work does not go into Core: a Store plugin or a Host-based
product carries it. That is why building the generic Host never needs a
third-party credential.

Automation talks to points and nothing else. One physical device is one point
(``protocol.device``), its parameters live in ``props.params``, and the adapter
keeps the protocol detail to itself. A rule that sends ``turn_on`` to
``sim.js_kitchen`` keeps working when that device later arrives through another
adapter under the same ``point_id``.

Changing configuration reloads the Hub rather than rebooting it. Editing
``adapters.json``, installing a ``.mhub`` or enabling a module calls
``minihub_hub_reload``: adapters stop and load again. Matter+Thread builds may
re-attach CHIP, so wait for Hub ready (MCP answers ``HUB_NOT_READY`` while a
reload runs).

.. only:: not hide_l3

   Reload also ``dlclose`` s ELF modules before they load again.

The generic Host writes to two partitions. ``storage`` mounts at ``/data`` (Hub
config **and** the Web console, at ``/data/web/``, plus the device icons under
``/data/images/``). The second one, ``modstore``, mounts at ``/store`` and holds
the Store tree — installed modules, packs and ``catalog.json`` — but only when
``CONFIG_MINIHUB_DYNAMIC_MODULES`` is on; with the option off there is no Store
root and Store downloads are disabled. ``/store/preinstall/web/`` is the
legacy console location that is still consulted as a fallback. A
full factory flash rewrites both, ``app-flash`` does not. See
:doc:`production`.

JSON authoring: :doc:`data_model` (points / adapters / rules); type catalog and
widgets: :doc:`type_catalog`. End-to-end flow (catalog, Overview page,
Store): :doc:`data_flow`.

Key components
--------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Component
     - What it owns while the Hub runs
   * - ``minihub_core``
     - Point registry, command router, event bus
   * - ``minihub_config``
     - Reading and writing ``/data/config/*.json``
   * - ``minihub_hub``
     - The soft reload sequence
   * - ``minihub_web``
     - HTTP REST, the Web console, ``/ws/events``
   * - ``minihub_module``
     - Reading manifests and loading modules
   * - ``minihub_pack``
     - JSON config packs, merged through ``inject``
   * - ``minihub_store``
     - Catalog, install, upgrade, uninstall
   * - ``minihub_net``
     - Ethernet and Wi-Fi STA
   * - ``minihub_matter``
     - Matter controller glue
   * - ``minihub_js``
     - The QuickJS worker and the ``host`` object
   * - ``minihub_device_widget``
     - Type catalog: CSV in, labels and widgets out
   * - ``minihub_adapter``
     - Adapter registry, radio start order, lifecycle lock, poll task
   * - ``minihub_protocol_adapters/adapter_*``
     - Static protocol stacks (wifi, zigbee, ble_mesh, matter, thread, mqtt,
       modbus)
   * - ``examples/plugins/ID/``
     - In-tree Store demos; extra source trees through ``MINIHUB_PLUGIN_DIRS``

.. only:: not hide_l3

   ``minihub_module`` also loads ELF ``.so`` modules through the ABI vtable.

Which folder holds what, and who is supposed to edit it, is in
:doc:`source_tree`.

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
   /data/config/packs/  Optional JSON pack staging (product-dependent)
   /store/modules/ID/   Installed Store modules (module.json, main.js, …)
   /data/web/           Host Web console (index.html, login.html, …)
   /data/tmp/           scratch (module loader)

NVS holds Wi-Fi credentials, Store URL overrides, the web password and API token.
Boot selection (``ota_0`` versus the ``app,factory`` Vibe Mode image) and
self-test state live in the ``otadata`` partition, not NVS. Neither holds the
full ``adapters.json`` — that lives on LittleFS.

Boot sequence (``application/minihub``)
---------------------------------------

Actual order in ``main`` (`application/minihub/main/main.c`, one call per line):

1. ``minihub_auth_init()``, ``minihub_ota_init()``, then ``minihub_core_init()``
   — point registry, event bus, command router
2. Optional on-device MCP init (Kconfig-gated)
3. ``minihub_config_init()`` — this is what mounts ``/data`` and ``/store``
4. Adapter registry init + static adapter registration; then the module loader
   init, ``minihub_module_load_all()``, and pack init/merge
5. ``minihub_adapter_load_all()`` — manifests are read before the network comes up
6. ``minihub_net_init()`` — Ethernet start; Wi-Fi **prepared only** (deferred STA)
7. ``minihub_thread_init()`` as built, rules and scenes load
8. ``minihub_web_start(80)`` — UI reachable while radios start
9. ``minihub_adapter_start_all()`` — Wi-Fi STA connect is the ``wifi`` adapter;
   the radio mutex skips the 802.15.4 adapter if Wi-Fi or BLE Mesh is enabled
   alongside it (see :doc:`radios`)
10. Matter↔Thread attach, ``minihub_js_reload()`` and the deferred hybrid module
    load, then USB MSC / RTSP / RainMaker / LCD as built, the adapter poll task,
    and the ``MiniHub ready`` log.

``wifi STA deferred`` in logs is expected until the ``wifi`` adapter starts.
See :doc:`networking`.

Hub soft reload
---------------

``POST /api/hub/reload`` calls ``minihub_hub_reload()`` synchronously; MCP
``hub.reload``, a Store install and an ``adapters.json`` write all go through
``minihub_hub_reload_async()`` on the ``hub_reload`` task. A mutex serializes
reloads so a second request waits up to 60 s. The steps, in
``minihub_hub_reload()`` order:

1. ``minihub_js_unload_all()`` first — stop JS handlers before adapters stop and
   config is re-read, so callbacks cannot keep touching points
2. Adapter lifecycle lock (60 s; ``ESP_ERR_TIMEOUT`` aborts the reload)
3. ``minihub_config_reload()`` — re-read JSON from flash (aborts on error)
4. ``minihub_pack_reload()`` — merge JSON packs into the adapters view
5. Stop all adapters; ``minihub_module_reload()`` — rescan manifests
6. ``minihub_adapter_load_all()`` + ``minihub_adapter_start_all()``
7. Unlock; Matter + Thread builds re-attach CHIP to OpenThread
8. JS unload + ``minihub_js_reload()`` (re-run ``setup(host)``), then
   ``minihub_module_load_deferred()`` for hybrid packages
9. ``minihub_rules_reload()`` + ``minihub_scenes_reload()``, then a
   ``hub.reloaded`` event on the bus — which is why a console re-fetches after a
   reload rather than trusting the HTTP reply.

The lifecycle lock is also what keeps ``adapter_poll`` from running across a
reload.

.. only:: not hide_l3

   The same lock covers ``dlclose`` of an ELF ``.so``.

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

   POST /api/store/install { type, id }
        → download .mhub → unzip under /store/modules/ID/
        → optional hub reload
        → minihub_module_load_all → minihub_module_entry → register adapter

See :doc:`plugins/pack_and_catalog`, :doc:`api/host_rest`, :doc:`api/plugin_rest`.

Configuration sources
---------------------

* **Build time:** Kconfig, ``manifest.yml``, ``sdkconfig_features``
* **Factory JSON:** ``application/APP/configs/adapters.json`` or repo ``configs/``
* **Runtime:** Web UI, REST ``PUT /api/config/adapters``,
  ``PUT /api/dev/catalog/…``, module RPC that calls
  ``config_save_adapters`` via host vtable

Secondary development surface (public contract)
-----------------------------------------------

The supported extension surface for a Host firmware you customize, and for Store
plugins.

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Surface
     - Chapter / artifact
   * - Product profile
     - ``manifest.yml``, ``partitions.csv``, ``configs/sdkconfig/app.yml`` —
       :doc:`host_customization`
   * - Runtime JSON / catalog
     - ``adapters.json``, ``rules.json``, ``scenes.json`` —
       :doc:`data_model`; type catalog CSV — :doc:`type_catalog`
   * - HTTP / events
     - :doc:`api/host_rest`; WebSocket events at ``/ws/events``
   * - MCP tools
     - :doc:`api/mcp`
   * - JS host object
     - :doc:`api/js_host`
   * - Store pack / catalog
     - :doc:`plugins/pack_and_catalog`
   * - Factory / OTA
     - :doc:`production`

.. only:: not hide_l3

   .. list-table::
      :header-rows: 1
      :widths: 28 72

      * - Surface
        - Chapter / artifact
      * - ELF module + adapter C API
        - :doc:`api/module_abi`, :doc:`api/minihub_core`, :doc:`api/minihub_adapter`
      * - ELF link symbols
        - ``docs/module_sdk/host_export_allowlist.json``

   **ABI stability:** ELF ``.so`` modules declare ``abi_version`` and
   ``core_api_min`` / ``core_api_max`` in ``module.json``, and Host refuses to
   install or load a mismatched one. Native modules are not published to the
   Store, so a release ships the Host firmware plus JSON/JS packs, assets, and the
   web console. Host export changes require
   allowlist and symbol regeneration (:doc:`plugins/develop_elf`).

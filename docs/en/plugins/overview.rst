Plugin Overview
===============

:link_to_translation:`zh_CN:[中文]`

Host vs plugin
--------------

* **In Host (static link):** Core, Web, MCP; Zigbee / BLE Mesh / Matter / MQTT / Modbus
  when compiled in via Kconfig
* **Via Store (``.mhub``):** ecosystem / vendor adapters as L1 or L2
  (choose a tier below)

.. only:: not hide_l3

   L3 is the native ELF tier (``.so`` inside ``.mhub``). Hybrid is L2 plus L3
   in one package.

Do not add vendor cloud credentials to generic Host Core ``configs/``. Tokens belong
in ``/store/modules/ID/`` or module-private NVS keys.

Module tiers
------------

Pick a tier from the constraints below, then open that chapter. Packing,
catalog, and install: :doc:`pack_and_catalog`.

.. list-table::
   :header-rows: 1
   :widths: 12 30 38 20

   * - Tier
     - In the package
     - When / constraints
     - How to build
   * - L1
     - ``runtime: pack`` plus ``inject``; no ``main.js``, no ``.so``.
       Merged into ``adapters.json`` at runtime.
     - **Use when:** default parameters for an existing **Host** adapter
       (BLE Mesh bind, UUID filters, Modbus maps). **Do not** inject radio
       ``devices[]`` (the Host registers one point per join). Keep vendor
       defaults out of Host Core ``configs/``.
       **Do not use when:** custom logic, cloud HTTP, a new adapter, or
       vendor claim / OTA.
     - :doc:`develop_l1`
   * - L2
     - ``runtime: js`` plus ``main.js``; optional ``meta.json`` / ``web/``.
     - **Use when:** lightweight automation, HTTP polling, REST/point glue,
       or a Web panel without native code.
       **Requires:** ``CONFIG_MINIHUB_DYNAMIC_MODULES`` (``dynamic_modules``
       in ``app.yml``).
     - :doc:`develop_l2`

.. only:: not hide_l3

   .. list-table::
      :header-rows: 1
      :widths: 12 30 38 20

      * - Tier
        - In the package
        - When / constraints
        - How to build
      * - L3
        - Omit ``runtime`` or set ``native``, plus an MHMD-wrapped ``.so``;
          optional ``meta.json`` / ``web/``. Host ``dlopen`` then
          ``minihub_module_entry``.
        - **Use when:** a new protocol or vendor adapter, C performance, or Host
          ABI. L2 can be a prototype before this tier. **Requires:** a matching
          cross compiler (or in-tree firmware build); ``abi_version`` /
          ``core_api_*`` must bracket the Host; symbols on the allowlist. See
          :doc:`../api/module_abi`.
          Example ``color_bulb``: ``module.json`` + ``color_bulb.so`` (+ optional
          ``devices.json``), no ``main.js``, no ``inject`` — that is L3, not L1.
        - :doc:`develop_l3`
      * - Hybrid
        - **L2 + L3 only:** same ``.mhub`` contains both ``.so`` and
          ``main.js``, ``runtime: hybrid``, and ``entry`` is exactly one of
          ``native`` or ``js``.
        - **Use when:** one module needs a native adapter and JS.
          This is **not** an arbitrary mix of L1 / L2 / L3, and **not**
          L1+L2, L1+L3, or all three in one package (``runtime: pack`` and
          ``hybrid`` are mutually exclusive). Ship a separate L1 module for
          ``inject``.
        - :doc:`develop_l3` and
          :doc:`develop_l2`

Every tier has ``module.json``; ``meta.json`` (``ui`` + ``capabilities``) is an optional sibling file.
That is **not** L1. L1 means ``inject`` merged by
``minihub_pack`` into an existing adapter block (for example BLE Mesh bind
defaults).

.. only:: not hide_l3

   ``meta.json`` is also valid on L3 / Hybrid. Installing L3 at most creates an
   empty ``adapters.json`` slot (``enabled`` + ``devices``); the module reads
   and writes its own config.

**Device model:** register one point per physical device
(``protocol.device``), put live values in ``props.params``. ``type`` and
``devices.json`` must match the Host type catalog for standard Devices-page
widgets (:doc:`../type_catalog`). Point fields and adapters / rules:
:doc:`../data_model`. Do not invent a C struct per lamp or switch. Demos:
L1 ``examples/plugins/ble_mesh_pack``, L2 ``color_bulb_js``.
Extra vendor plugins live outside this tree —
export ``MINIHUB_PLUGIN_DIRS`` at a sibling Store plugin repo before configure.

.. only:: not hide_l3

   L3 demos: ``examples/plugins/color_bulb``, ``examples/plugins/meter_plug``.

Source tree
-----------

Optional in-repo sources:

* Demos: ``examples/plugins/ID/``
  (export ``MINIHUB_PLUGIN_DIRS`` **before** CMake for extras). Index:
  ``examples/README.md``. Public Host ``store_preinstall`` defaults to empty.

On device after install:

.. code-block:: text

   /store/modules/ID/
     module.json      # every tier (not L1)
     main.js          # L2
     meta.json         # optional ui + capabilities
     .load_guard      # written by Host during load

.. only:: not hide_l3

   .. code-block:: text

      ID.so            # L3 / Hybrid (name from module.json "binary")
      meta.json         # also used by L3 / Hybrid

Deploy / catalog (development PC):

.. code-block:: text

   $MINIHUB_PLUGINS_DEPLOY_DIR/          # default ~/workspace/minihub_store
     Stores/Common/ID/catalog.json
     Stores/Common/ID/ID.mhub

L1 and L2 use that layout. A native module is published under
``Stores/<chip>/<id>/`` (``<chip>`` from ``targets``); L1/L2 use ``Common``.

.. _store-preinstall:

Factory preinstall
------------------

List module ids in ``application/APP/manifest.yml``:

.. code-block:: yaml

   store_preinstall:
     - color_bulb_js

.. only:: not hide_l3

   An L3 id such as ``color_bulb`` is listed the same way.

Firmware build copies packed ``.mhub`` files into ``modstore`` (mounted as
``/store``). The ``preinstall`` field in ``module.json`` is not used for factory
bundling.

Development loop (summary)
--------------------------

1. Edit sources under ``examples/plugins/ID/`` (or
   ``MINIHUB_PLUGIN_DIRS``)
2. ``python3 tools/pack_store_module.py pack ID``
3. After ``.mhub`` is in the store directory:
   ``python3 tools/sync_store_catalog.py`` then
   ``python3 tools/store_server.py``
4. On Hub: install from catalog or upload ``.mhub``, then ``POST /api/hub/reload``

.. only:: not hide_l3

   For L3, compile the ``.so`` first (standalone cross-compile or with the Host;
   see :doc:`develop_l3`), then pack that id.

Scaffolding for the tier you picked is linked in the previous section.
Catalog and publish: :doc:`pack_and_catalog`.

.. only:: hide_l3

   Developer scripts (L1 / L2)
   ---------------------------

   .. list-table::
      :header-rows: 1
      :widths: 44 10 10 36

      * - Script
        - L1
        - L2
        - Role
      * - ``tools/new_store_module.py --tier l1|l2``
        - yes
        - yes
        - Scaffold under ``examples/plugins/``
      * - ``tools/pack_store_module.py pack ID|all``
        - yes
        - yes
        - Zip ``.mhub`` (does not compile C)
      * - ``tools/sync_store_catalog.py``
        - yes
        - yes
        - Rewrite ``catalog.json`` beside each deployed ``.mhub`` (default)
      * - ``tools/sync_store_catalog.py all``
        - yes
        - yes
        - Pack from sources, then rewrite ``catalog.json`` the same way
      * - ``tools/store_server.py``
        - yes
        - yes
        - Local catalog HTTP server
      * - ``tools/minihub_plugin_mcp.py`` (IDE MCP)
        - yes
        - yes
        - Scaffold / validate / pack

   Tier details: :doc:`develop_l1`, :doc:`develop_l2`.

.. only:: not hide_l3

   Developer scripts (L1 / L2 / L3)
   --------------------------------

   .. list-table::
      :header-rows: 1
      :widths: 40 8 8 8 36

      * - Script
        - L1
        - L2
        - L3
        - Role
      * - ``tools/new_store_module.py --tier l1|l2|l3``
        - yes
        - yes
        - yes
        - Scaffold under ``examples/plugins/``
      * - ``tools/pack_store_module.py pack ID|all``
        - yes
        - yes
        - yes
        - Zip ``.mhub`` (does not compile C)
      * - ``tools/pack_store_module.py mhmd``
        - —
        - —
        - yes
        - Wrap ELF with MHMD header
      * - ``tools/sync_store_catalog.py``
        - yes
        - yes
        - yes
        - Rewrite ``catalog.json`` beside each deployed ``.mhub`` (default)
      * - ``tools/sync_store_catalog.py all``
        - yes
        - yes
        - yes
        - Pack from sources, then rewrite ``catalog.json`` the same way
      * - ``tools/store_server.py``
        - yes
        - yes
        - yes
        - Local catalog HTTP server
      * - ``tools/generate_devices_property_from_json.py``
        - —
        - —
        - opt
        - ``devices.json`` → ``device_desc.gen.h``
      * - ``tools/build_firmware.py … build`` / ``modules``
        - —
        - —
        - yes
        - Compile in-tree native ``.so`` (``modules``) or pack (``pack``)
      * - ``tools/build_plugin_standalone.py ID``
        - —
        - —
        - yes
        - No-IDF CMake build for ``examples/plugins`` L3 demos
      * - ``tools/minihub_plugin_mcp.py`` (IDE MCP)
        - yes
        - yes
        - yes
        - Scaffold / validate / pack; does not cross-compile ``.so``
      * - ``tools/check_plugin_sdk.py``
        - —
        - —
        - yes
        - Host-side SDK vs allowlist check

   Tier details: :doc:`develop_l1`, :doc:`develop_l2`, :doc:`develop_l3`.

``module.json`` fields
----------------------

Required for every tier:

* ``id`` — catalog and directory name (also Store REST ``id``)
* ``name`` · ``version`` — shown in catalog / ``GET /api/modules``
* ``enabled`` — default on after install

Tier / loader:

* ``runtime`` — ``pack`` (L1) or ``js`` (L2)
* ``adapter`` — ``adapters.json`` key Host will create/enable
* ``ui`` / ``capabilities`` — live in ``meta.json`` (a sibling file), **not** in
  module.json. module.json carries only runtime/environment info. ``ui`` is the
  optional Web form; ``capabilities`` are informational tags
  (``points``, ``auth``, ``device_sync``)
* ``vendor`` · ``description`` — catalog text
* ``inject`` — L1 merge payload only
* ``preinstall`` — **ignored** for factory bundling; use
  ``store_preinstall`` in :ref:`store-preinstall` instead

.. only:: not hide_l3

   Also for L3 / Hybrid:

   * ``runtime`` — omitted/native (L3), or ``hybrid`` (L2 + L3 in one package
     only; see the tier table)
   * ``binary`` — L3 filename inside the unpacked dir (example ``color_bulb.so``)
   * ``abi_version`` · ``core_api_min`` · ``core_api_max`` — must bracket Host
     (see :doc:`../api/module_abi`)
   * ``inject`` is not loaded for Hybrid / L3 packages (``minihub_pack``
     loads L1 only)

Type catalog and ``devices.json``: :doc:`../type_catalog`.
JSON authoring for adapters/rules/scenes: :doc:`../data_model`.
Factory images: :doc:`../production`.

.. only:: not hide_l3

   ABI contract
   ------------

   L3 modules must honor ``docs/module_sdk/host_export_allowlist.json`` and
   ``minihub_module_abi.h``. See :doc:`../api/module_abi`.

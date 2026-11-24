Plugin Overview
===============

:link_to_translation:`zh_CN:[中文]`

Host vs plugin
--------------

Code that ships with every unit is linked into the firmware: Core, and the radio
stacks and adapters that Kconfig enables (Zigbee, BLE Mesh, Matter, MQTT,
Modbus). The web console is a linked HTTP server plus the HTML, CSS and
JavaScript that live on the ``storage`` partition, so a re-skin does not rebuild
the app (:doc:`../web_console`). The MCP server is its own component and is on
by default (``CONFIG_MINIHUB_MCP_SERVER``).

Developer and ecosystem adapters are different. They often carry credentials that
must not appear in a public build, and not every product needs them. Those arrive
later as Store ``.mhub`` packages — either a JSON (config-only) or a JS
package (choose a tier below).

.. only:: not hide_l3

   ELF is the native (``.so``) tier, wrapped inside ``.mhub``. Hybrid is JS plus
   ELF in one package.

Do not put vendor cloud credentials in the generic Host Core ``configs/``.
Anything written there is flashed into every unit of a public build. Keep tokens
under ``/store/modules/ID/`` or in module-private NVS keys.

Module tiers
------------

Pick a tier from the constraints below, then open that chapter. Packing, catalog
and install work the same way for every tier (:doc:`pack_and_catalog`). To follow
one plugin from an empty directory to a running device, see :doc:`first_plugin`.
Each tier also has a project skill for AI assistants under ``.agent/skills/``,
named after the tier and listed in :doc:`../vibe_coding`.

Choose by what you already write:

* **JSON** -- configuration only, no code at all: a JSON pack that adds defaults to
  an adapter the Host already ships.
* **JS** -- JavaScript on the embedded **QuickJS-NG** engine. For developers
  comfortable in JS: no cross-compiler, no ``.so``, and a fault in one module
  cannot take down the Host. It is not a browser, so read the runtime limits in
  :doc:`develop_js` first.

.. only:: not hide_l3

   * **ELF** -- native C/C++ compiled to a ``.so`` and loaded through the Host ABI
     vtable. For embedded developers who need the export allowlist, threads, or a
     binary protocol.

.. list-table::
   :header-rows: 1
   :widths: 12 30 38 20

   * - Tier
     - In the package
     - When / constraints
     - How to build
   * - JSON
     - ``runtime: pack`` plus ``inject``; no ``main.js``, no ``.so``.
       Merged into ``adapters.json`` at runtime.
     - **Use when:** default parameters for an existing **Host** adapter
       (BLE Mesh bind, UUID filters, Modbus maps). **Do not** inject radio
       ``devices[]`` (the Host registers one point per join). Keep your own
       defaults out of Host Core ``configs/``.
       **Do not use when:** custom logic, cloud HTTP, a new adapter, or
       plugin associate / OTA.
     - :doc:`develop_json`
   * - JS
     - ``runtime: js``: ``main.js`` run by the embedded **QuickJS** engine;
       optional ``meta.json`` / ``web/``.
     - **Use when:** automation, HTTP polling, REST / point glue, or a Web panel
       -- in JavaScript, with no native code and no cross-compile step.
       **Requires:** ``CONFIG_MINIHUB_DYNAMIC_MODULES`` and
       ``CONFIG_MINIHUB_JS_PLUGINS`` (``dynamic_modules`` in ``app.yml``).
       The engine is single-threaded: one callback at a time for all JS modules,
       so long work must yield (``host.adapter.poll`` / ``setTimeout``).
     - :doc:`develop_js`

.. only:: not hide_l3

   .. list-table::
      :header-rows: 1
      :widths: 12 30 38 20

      * - Tier
        - In the package
        - When / constraints
        - How to build
      * - ELF
        - Omit ``runtime`` or set ``native``, plus an MHMD-wrapped ``.so``;
          optional ``meta.json`` / ``web/``. Host ``dlopen`` then
          ``minihub_module_entry``.
        - **Use when:** a new protocol or vendor adapter, C performance, or Host
          ABI. JS can be a prototype before this tier. **Requires:** a matching
          cross compiler (or in-tree firmware build); ``abi_version`` /
          ``core_api_*`` must bracket the Host; symbols on the allowlist. See
          :doc:`../api/module_abi`.
          Example ``my_native``: ``module.json`` + ``my_native.so`` (+ optional
          ``devices.json``), no ``main.js``, no ``inject`` — that is ELF, not JSON.
        - :doc:`develop_elf`
      * - Hybrid
        - **JS + ELF only:** same ``.mhub`` contains both ``.so`` and
          ``main.js``, ``runtime: hybrid``, and ``entry`` is exactly one of
          ``native`` or ``JS``.
        - **Use when:** one module needs a native adapter and JS.
          This is **not** an arbitrary mix of JSON / JS / ELF, and **not**
          JSON+JS, JSON+ELF, or all three in one package (``runtime: pack`` and
          ``hybrid`` are mutually exclusive). Ship a separate JSON module for
          ``inject``.
        - :doc:`develop_elf` and
          :doc:`develop_js`

Every tier has a ``module.json``. Any tier may also ship a sibling ``meta.json``
for ``ui`` and ``capabilities``. Having a ``module.json`` does not make the
package JSON. JSON means ``inject`` is merged by ``minihub_pack`` into an existing
adapter block — BLE Mesh bind defaults are the usual example.

.. only:: not hide_l3

   ``meta.json`` is also valid on ELF / Hybrid. Installing ELF at most creates an
   empty ``adapters.json`` slot (``enabled`` + ``devices``); the module reads
   and writes its own config.

Every tier uses the same device model. One physical device is one point
(``protocol.device``), with live values under ``props.params``. The ``type``
string and any ``devices.json`` must match the Host type catalog, or the Devices
page has no widget to draw (:doc:`../type_catalog`). Point fields, adapters and
rules are in :doc:`../data_model`. Do not invent a C struct per lamp or switch —
a type is a name plus parameters, so a new device family needs no firmware
change. Demos: JSON ``examples/plugins/ble_mesh_pack``, JS ``color_bulb_js``.
Third-party plugins live outside this tree; point ``MINIHUB_PLUGIN_DIRS`` at a sibling
Store plugin repository before CMake runs.

.. only:: not hide_l3

   There is no in-tree ELF demo; scaffold one with
   ``tools/new_store_module.py --tier l3 --id my_native``.

Source tree
-----------

In-repo sources are optional. A plugin can live entirely in a sibling repository:

* Demos: ``examples/plugins/ID/``
  (export ``MINIHUB_PLUGIN_DIRS`` **before** CMake for extras). Index:
  ``examples/README.md``. Public Host ``store_preinstall`` defaults to empty.

On device after install:

.. code-block:: text

   /store/modules/ID/
     module.json      # every tier (not JSON)
     main.js          # JS
     meta.json         # optional ui + capabilities
     .load_guard      # written by Host during load

.. only:: not hide_l3

   .. code-block:: text

      ID.so            # ELF / Hybrid (name from module.json "binary")
      meta.json         # also used by ELF / Hybrid

Deploy / catalog (development PC):

.. code-block:: text

   $MINIHUB_PLUGINS_DEPLOY_DIR/          # default ~/workspace/minihub_store
     Stores/Common/ID/catalog.json
     Stores/Common/ID/ID.mhub

JSON and JS use that layout. A native or hybrid module is not published. The
packer creates no store folder for it. Any folder left by an earlier build is
removed only during catalog sync that moves packages, not by the packer.

.. _store-preinstall:

Factory preinstall
------------------

Factory bundling is a product decision, not a module one. List the module ids in
``application/APP/manifest.yml``:

.. code-block:: yaml

   store_preinstall:
     - color_bulb_js

.. only:: not hide_l3

   An ELF id such as ``my_native`` is listed the same way.

The firmware build copies each packed ``.mhub`` into ``modstore`` (mounted as
``/store``). The ``preinstall`` field inside ``module.json`` is ignored for
factory bundling.

Developer scripts
-----------------

.. list-table::
   :header-rows: 1
   :widths: 46 54

   * - Script
     - Role
   * - ``tools/new_store_module.py --tier l1|l2 --id ID``
     - Scaffold under ``examples/plugins/``
   * - ``tools/pack_store_module.py pack ID|all``
     - Zip ``.mhub`` (does not compile C)
   * - ``tools/sync_store_catalog.py``
     - Rewrite ``catalog.json`` beside each deployed ``.mhub`` (default)
   * - ``tools/sync_store_catalog.py all``
     - Pack from sources, then rewrite ``catalog.json`` the same way
   * - ``tools/store_server.py``
     - Local catalog HTTP server
   * - ``tools/minihub_plugin_mcp.py`` (IDE MCP)
     - Scaffold / validate / pack; does not cross-compile ``.so``

Tier details: :doc:`develop_json`, :doc:`develop_js`.

.. only:: not hide_l3

   ELF adds ``tools/new_store_module.py --tier l3``,
   ``tools/pack_store_module.py mhmd`` (wrap an ELF with the MHMD header),
   ``tools/build_firmware.py modules``,
   ``tools/build_plugin_standalone.py ID`` (no-IDF CMake build),
   ``tools/generate_devices_property_from_json.py`` (``devices.json`` →
   ``device_desc.gen.h``, optional) and ``tools/check_plugin_sdk.py check``
   (also `coverage` and `emit-sizes`; the Host CMake runs `check` itself when
   ``CONFIG_MINIHUB_DYNAMIC_MODULES`` is on). See :doc:`develop_elf`.

``module.json`` fields
----------------------

Every tier ships a ``module.json``. Two fields are what the loader actually
insists on:

* ``id`` — catalog and directory name (also Store REST ``id``). At install it must
  equal the catalog entry's ``id``.
* ``adapter`` — the ``adapters.json`` key Host creates and enables. A missing or
  empty ``adapter`` fails validation for **every** tier, JS included, so a JS
  plugin without it never loads.

**``requires``** is the capability gate for the Store. It is a list of tokens
from ``Zigbee``, ``BLE_mesh``, ``Thread_Matter``, ``Matter``, ``MQTT_Broker``,
and ``Modbus``. The Hub sends those tokens as ``caps=`` on ``GET /api/catalog``.
The store omits a not-yet-installed plugin while any listed token is missing on
that Hub. ``[]`` or an omitted field means no gate. This is not the same as
``meta.json`` ``capabilities`` (``points``, ``auth``, ``device_sync``), which are
informational only and do not filter the catalog.

Everything else has a default, so leaving it out is not an error:

* ``name`` · ``version`` — shown in catalog / ``GET /api/modules``; ``version``
  falls back to ``0.0.0``
* ``enabled`` — defaults to true
* ``abi_version`` · ``core_api_min`` — default to the Host's own values;
  ``core_api_max`` defaults to 0, meaning no upper bound
* ``runtime`` — ``pack`` (JSON) or ``JS`` (JS)
* ``requires`` — defaults to ``[]`` when omitted; pack copies it onto
  ``catalog.json``

``ui`` / ``capabilities`` are read from ``meta.json`` when a sibling ``meta.json``
exists, otherwise from ``module.json`` itself; ``ui`` is the optional Web form and
``capabilities`` are informational tags (``points``, ``auth``, ``device_sync``).

* ``vendor`` · ``description`` — catalog text
* ``inject`` — JSON merge payload only
* ``preinstall`` — **ignored** for factory bundling; use
  ``store_preinstall`` in :ref:`store-preinstall` instead

.. only:: not hide_l3

   Also for ELF / Hybrid:

   * ``runtime`` — omitted/native (ELF), or ``hybrid`` (JS + ELF in one package
     only; see the tier table)
   * ``binary`` — ELF filename inside the unpacked dir (example ``my_native.so``)
   * ``abi_version`` · ``core_api_min`` · ``core_api_max`` — must bracket Host
     (see :doc:`../api/module_abi`)
   * ``inject`` is not loaded for Hybrid / ELF packages (``minihub_pack``
     loads JSON only)

Type catalog and ``devices.json``: :doc:`../type_catalog`.
JSON authoring for adapters/rules/scenes: :doc:`../data_model`.
Factory images: :doc:`../production`.

.. only:: not hide_l3

   ABI contract
   ------------

   ELF modules must honor ``docs/module_sdk/host_export_allowlist.json`` and
   ``minihub_module_abi.h``. See :doc:`../api/module_abi`.

Develop L3 (Native) Modules
===========================

:link_to_translation:`zh_CN:[中文]`

L3 modules ship a **native ELF** ``.so`` inside the outer ``.mhub`` zip. The
loader expects an **MHMD** header on the ``.so`` payload (see
``tools/pack_store_module.py mhmd``). Host ``dlopen`` s the image and calls
``minihub_module_entry``.

End-to-end workflow
-------------------

1. **Scaffold** — ``module.json``, sources, optional ``ui.json``
2. **Compile** the ``.so`` — pick one of the two methods below
3. **Pack** — ``python3 tools/pack_store_module.py pack ID``
4. **Catalog** — after ``.mhub`` files are in the store directory, rewrite
   ``catalog.json`` (the Hub lists modules from that file, not by scanning
   zips): ``python3 tools/sync_store_catalog.py --from-store``
5. **Install on device** — Store UI, ``POST /api/store/install``, or factory
   ``store_preinstall``
6. **Reload** — ``POST /api/hub/reload``; check serial + ``GET /api/modules``

Two ways to compile
--------------------

Out-of-tree (standalone)
^^^^^^^^^^^^^^^^^^^^^^^^

No ESP-IDF and no MiniHub source tree. Use Plugin SDK headers
(``#include "minihub_plugin_sdk.h"``) and a cross compiler that matches the Host
(ESP32-S31: ``riscv32-esp-elf-gcc``). Do not mix SDK headers with IDF headers in
the same translation unit. Header reference: :doc:`../api/plugin_host`
(:download:`module_sdk.zip <../../_static/module_sdk.zip>`).

.. code-block:: bash

   # 1. Compile the raw ELF (intermediate)
   mkdir -p build/lib
   riscv32-esp-elf-gcc -std=gnu17 -shared -fPIC \
     -nostdlib -nostartfiles \
     -fdata-sections -ffunction-sections -fvisibility=hidden \
     -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4 \
     -Wl,--gc-sections -Wl,--allow-shlib-undefined \
     -Wl,-u,minihub_module_entry \
     -I /path/to/module_sdk/include \
     -o build/lib/my_native.so module_entry.c my_native_adapter.c

   # 2. MHMD header — final .so stays in this plugin's build/
   #    pack_store_module.py and store_paths.py ship inside module_sdk.zip
   python3 /path/to/module_sdk/pack_store_module.py mhmd \
     build/lib/my_native.so build/my_native.so

   # 3. Pack .mhub (required; the Hub installs the zip, not the bare .so)
   export MINIHUB_PLUGIN_DIRS="$PWD"
   python3 /path/to/module_sdk/pack_store_module.py pack my_native

   # 4. Rewrite catalog.json from each Stores/ESP32S31 or Stores/Common .mhub.
   #    sync_store_catalog.py ships in module_sdk.zip next to pack_store_module.py.
   python3 /path/to/module_sdk/sync_store_catalog.py --from-store \
     --deploy ~/workspace/minihub_store

``-shared -fPIC`` and ``-Wl,-z,max-page-size=4`` are required for ``dlopen`` on
ESP32-S31. With ``-fvisibility=hidden`` and ``--gc-sections``, also pass
``-Wl,-u,minihub_module_entry`` (and compile ``module_entry.c`` with default
visibility) so the loader entry is not discarded. Do not link the Host ELF.
Undefined symbols in the ``.so`` must be
in ``docs/module_sdk/host_export_allowlist.json``; Host resolves them at load.
Declarations come from ``minihub_plugin_sdk.h``; do not write ``extern`` by
hand. **Excluded:** radio-stack APIs and httpd URI registration. See
:ref:`l3-host-export-allowlist`.

Standalone CMake fragment (copy into your project)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Prefer CMake over a raw ``gcc`` one-liner when you have several sources or
``devices.json``. Take ``minihub_plugin_standalone.cmake`` from
:download:`module_sdk.zip <../../_static/module_sdk.zip>`, put it **inside** the
plugin directory (same folder as ``CMakeLists.txt``, or ``cmake/`` under that
directory — not beside the plugin folder). Defaults: ``MINIHUB_MODULE_ID`` is
the plugin directory name; ``MINIHUB_MODULE_SRCS`` is every ``*.c`` and
``*.cpp`` in that directory (not subfolders). Override either variable before
``include()`` if needed. Third-party trees also set ``MINIHUB_SDK_INCLUDE``.
Then:

.. code-block:: bash

   cmake -S . -B build \
     -DCMAKE_C_COMPILER=riscv32-esp-elf-gcc \
     -DCMAKE_SYSTEM_NAME=Generic \
     -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
   cmake --build build

The first command configures (required once, and again if ``CMakeLists.txt``
changes). The second compiles. After the first configure, source edits only
need ``cmake --build build``. The MHMD file is ``build/<id>.so``.

Pack that file into ``.mhub`` (plugin directory as the current directory; the
pack script lives in a MiniHub checkout):

.. code-block:: bash

   export MINIHUB_PLUGIN_DIRS="$PWD"
   python3 /path/to/module_sdk/pack_store_module.py pack "$(basename "$PWD")"
   python3 /path/to/module_sdk/sync_store_catalog.py --from-store \
     --deploy ~/workspace/minihub_store

Prefer ``-B build`` inside the plugin directory.
``build_plugin_standalone.py`` (in this tree: ``tools/`` or
``docs/module_sdk/``; in ``module_sdk.zip`` next to the cmake fragment) does
configure, build, and can ``--pack``. Host in-tree builds still write
``application/minihub/build/<id>.so``.

Example demos under ``examples/plugins/`` (dual path)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``color_bulb``, ``meter_plug``, ``zigbee_ota``, ``ble_mesh_ota``, and
``matter_ota`` each use ``if(ESP_PLATFORM)`` / standalone ``include()`` of the
fragment above. Without IDF:

.. code-block:: bash

   python3 tools/build_plugin_standalone.py color_bulb
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack color_bulb
   python3 tools/sync_store_catalog.py --from-store

In-tree with the Host
^^^^^^^^^^^^^^^^^^^^^^^

Sources live in ``examples/plugins/ID/`` (demos) or a tree listed in
``MINIHUB_PLUGIN_DIRS`` (export **before** CMake) with ``idf_component_register``.
``REQUIRES`` only the public libs you call. With
``CONFIG_MINIHUB_DYNAMIC_MODULES``, add ``-fPIC``. Firmware build uses
``cmake/minihub_module_so.cmake`` to produce
``application/minihub/build/modules/libadapter_ID.so``, then MHMD + pack.

.. code-block:: bash

   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # demos only
   ./tools/build_firmware.py -app minihub build

To pack again after the ``.so`` exists: ``python3 tools/pack_store_module.py pack ID``.

L3 developer scripts
^^^^^^^^^^^^^^^^^^^^

* ``tools/new_store_module.py --tier l3 --id ID`` — scaffold
* ``tools/generate_devices_property_from_json.py`` — ``devices.json`` →
  ``device_desc.gen.h`` (when used)
* ``tools/build_plugin_standalone.py ID`` — no-IDF CMake build for example demos
* ``tools/pack_store_module.py mhmd`` / ``pack`` — MHMD wrap + ``.mhub``
* ``tools/sync_store_catalog.py --from-store`` — after ``.mhub`` files are in
  the store directory, rewrite ``catalog.json`` (also in ``module_sdk.zip``)
* ``tools/build_firmware.py … modules`` — compile every in-tree native ``.so``; ``pack`` wraps ``.mhub``
* ``tools/check_plugin_sdk.py`` — Host-side SDK vs allowlist check
* IDE MCP: ``python3 tools/minihub_plugin_mcp.py`` (scaffold / validate / pack;
  does not cross-compile ``.so``)

See also :doc:`pack_and_catalog` and :doc:`overview`.

Directory layout (scaffold ``my_native``)
-----------------------------------------

.. code-block:: text

   examples/plugins/my_native/
     module.json
     module_entry.c       # exports minihub_module_entry (+ optional minihub_module_rpc)
     my_native_adapter.c  # minihub_adapter_t implementation
     CMakeLists.txt
     ui.json              # optional Web UI

``module.json`` (L3)
--------------------

.. code-block:: json

   {
     "id": "my_native",
     "name": "Example native adapter",
     "version": "0.1.0",
     "enabled": true,
     "adapter": "my_native",
     "binary": "my_native.so",
     "ui_file": "ui.json",
     "abi_version": 1,
     "core_api_min": 1,
     "core_api_max": 1,
     "capabilities": ["auth", "device_sync"]
   }

``binary`` is the filename **inside** the unpacked module directory on device
(for example ``/store/modules/my_native/my_native.so``).

``module_entry.c`` pattern
--------------------------

.. code-block:: c

   #include "minihub_plugin_sdk.h"

   extern const minihub_adapter_t my_adapter;

   esp_err_t minihub_module_entry(
       const minihub_module_host_api_t *host,
       const cJSON *config,
       const minihub_adapter_t **out_adapter)
   {
       if (!out_adapter) {
           return ESP_ERR_INVALID_ARG;
       }
       my_module_set_host(host);   /* save vtable for points/commands */
       (void)config;
       *out_adapter = &my_adapter;
       return ESP_OK;
   }

Optional RPC (``POST /api/modules/ID/rpc``):

.. code-block:: c

   esp_err_t minihub_module_rpc(const char *method, const cJSON *params, cJSON **out);

Host vtable (``minihub_module_host_api_t``)
-------------------------------------------

Modules must call Host only through the table passed to ``minihub_module_entry``:

* ``point_register`` / ``point_unregister`` / ``point_get`` / ``point_set`` —
  one device one point; put live params in ``props`` (``params.NAME``).
  Prefer ``minihub_point_merge_params`` / ``minihub_point_merge_props``
  (allowlisted) when updating telemetry or extras so other top-level keys
  (``ota``, ``ieee``, …) are preserved
* ``command_register_adapter``
* ``event_publish`` / ``event_subscribe`` — subscribe at least to
  ``point.updated`` / ``point.registered`` to enrich the same Host
  ``point_id`` (do not register a second point for the same lamp)
* ``config_get_adapters`` / ``config_save_adapters``
* ``hub_reload`` / ``adapter_soft_reload``

Headers: ``#include "minihub_plugin_sdk.h"`` — :doc:`../api/plugin_host`.
Vtable types are in that SDK header (and :doc:`../api/module_abi`).

Symbol allowlist
----------------

Undefined symbols in the ``.so`` must resolve against Host exports listed in
``docs/module_sdk/host_export_allowlist.json``. Declarations come from
``minihub_plugin_sdk.h``; do not write ``extern`` by hand. **Excluded:**
Wi-Fi / Zigbee / BLE Mesh / Matter radio APIs and httpd URI registration.
Full list: :ref:`l3-host-export-allowlist`.

After changing Host exports:

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

Host build also runs ``tools/check_plugin_sdk.py`` so ``minihub_plugin_sdk.h``
cannot drift from IDF/MiniHub layouts or the allowlist.

Install and debug
-----------------

.. code-block:: bash

   curl -X POST http://$HUB/api/store/install -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"my_native"}'
   curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
     -d '{"id":"my_native","enabled":true,"reload":true}'

If load crashes, Host writes ``/store/modules/my_native/.load_guard`` = ``loading``
and skips the module on next boot. Clear it:

.. code-block:: bash

   curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
     -d '{"id":"my_native","clear_load_guard":true,"reload":true}'

Common failures:

* ``abi_version`` mismatch — align with ``MINIHUB_MODULE_ABI_VERSION`` in header
* Relocation / undefined symbol — check allowlist; do not call ``esp_wifi_*`` from module
* ``binary_size`` mismatch after OTA — repack ``.mhub`` after rebuild

Device types, ``devices.json``, and widgets: :doc:`../type_catalog`
(demos ``examples/plugins/color_bulb``, ``examples/plugins/meter_plug``).
Point identity (manufacturer, category, OTA, ``owner``) and adapters JSON:
:doc:`../data_model`.

Header reference: :doc:`../api/plugin_host`, :doc:`../api/module_abi`,
:doc:`../api/minihub_core`, :doc:`../api/minihub_adapter`.

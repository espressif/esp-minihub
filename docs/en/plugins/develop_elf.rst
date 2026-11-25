Develop ELF (Native) Plugins
============================

:link_to_translation:`zh_CN:[中文]`

An ELF module is a ``.mhub`` package whose payload is a native ELF ``.so``. Host
calls ``dlopen`` on that image and then ``minihub_module_entry``, so the binary
must be built for the exact Host that will load it — nothing translates between
ABIs at install time. Before packing, wrap the ``.so`` with an **MHMD** header
(``tools/pack_store_module.py mhmd``); the loader recognises the image by that
header.

End-to-end workflow
-------------------

Packing a native module takes two steps more than packing a JS one: the ``.so``
must be compiled and wrapped before there is anything to zip.

1. **Scaffold** — ``module.json``, sources, optional ``meta.json``
2. **Compile** the ``.so`` — pick one of the two methods below
3. **Pack** — there is no such step for a native module:
   ``tools/pack_store_module.py pack ID`` skips ``runtime: native`` and produces
   no ``.mhub`` at all (``pack_store_module.py:236-239``). The MHMD wrap itself is
   ``tools/pack_store_module.py mhmd in.so out.so``.
4. **Publish** — ``tools/sync_store_catalog.py`` gives a native module no
   ``Stores/`` folder, and ``all`` (repacking from source) deletes one left over
   from an earlier build; ``--from-store`` leaves it alone, so a hand-written entry
   survives. To install from the Store you place ``ID.mhub`` in
   ``Stores/Common/ID/`` and write that directory's ``catalog.json`` yourself —
   the device does install native packages, the tooling just refuses to publish
   them.
5. **Onto the device** — production uses factory ``store_preinstall``:
   ``module.json`` and the MHMD ``.so`` are staged into the ``modstore`` image,
   and boot's ``scan_manifests`` loads them — no reload to press. If you place the
   files under ``/store/modules/ID/`` yourself (``PUT /api/dev/fs/`` cannot write
   there, so this means reflashing ``modstore`` or a shell on the device), nothing
   watches that directory: you need ``POST /api/hub/reload``. Installing from the
   Store reloads as it goes.
6. **Verify** — ``module 'ID' load ok`` on serial, and ``loaded`` /
   ``load_blocked`` / ``binary_stale`` / ``abi_version`` in ``GET /api/modules``.
   A bare ``.so`` is not enough: a directory without ``module.json`` is skipped by
   the scan, silently.

Two ways to compile
--------------------

Which one you use depends on whether the MiniHub tree is available to you.

Out-of-tree (standalone)
^^^^^^^^^^^^^^^^^^^^^^^^

This path needs no ESP-IDF and no MiniHub source tree. A developer can build against
the Plugin SDK headers alone (``#include "minihub_plugin_sdk.h"``) with a cross
compiler that matches the Host (ESP32-S31: ``riscv32-esp-elf-gcc``). Those
headers describe the Host as it is
exported, so mixing them with IDF headers in one translation unit gives two
conflicting pictures of the same symbols — do not. Header reference:
:doc:`../api/plugin_host`
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

   # 3. MHMD stays in this plugin's build/. pack does not create a store folder.
   export MINIHUB_PLUGIN_DIRS="$PWD"
   python3 /path/to/module_sdk/pack_store_module.py pack my_native

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

A scaffolded ELF plugin (dual path)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Splitting the source with ``if(ESP_PLATFORM)`` / a standalone ``include()`` of
the fragment above is what lets one tree build both ways. Without IDF:

.. code-block:: bash

   python3 tools/new_store_module.py --tier l3 --id my_native
   python3 tools/build_plugin_standalone.py my_native
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack my_native
   python3 tools/sync_store_catalog.py --from-store

In-tree with the Host
^^^^^^^^^^^^^^^^^^^^^^^

Sources live in ``examples/plugins/ID/`` (demos) or a tree listed in
``MINIHUB_PLUGIN_DIRS`` (export **before** CMake) with ``idf_component_register``.
``REQUIRES`` only the public libs you call. With
``CONFIG_MINIHUB_DYNAMIC_MODULES``, add ``-fPIC``.
``cmake/minihub_module_so.cmake`` produces
``application/minihub/build/modules/libadapter_ID.so``, then MHMD + pack,
when you build the module target. A Host firmware ``build`` does not.

.. code-block:: bash

   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # demos only
   ./tools/build_firmware.py -app minihub modules

With an MHMD ``.so`` already built there is nothing to recompile:
``python3 tools/pack_store_module.py mhmd in.so out.so`` only wraps the header.
``pack`` publishes JSON and JS — a native package either goes into the store folder
yourself (steps 3-5 above) or rides in the factory image.

ELF developer scripts
^^^^^^^^^^^^^^^^^^^^^^

* ``tools/new_store_module.py --tier l3 --id ID`` — scaffold
* ``tools/generate_devices_property_from_json.py`` — ``devices.json`` →
  ``device_desc.gen.h`` (when used)
* ``tools/build_plugin_standalone.py ID`` — no-IDF CMake build for example demos
* ``tools/pack_store_module.py mhmd`` — the MHMD wrap ( ``pack`` skips a native
  module and emits no ``.mhub``)
* ``tools/sync_store_catalog.py --from-store`` — rewrite ``catalog.json`` for the
  ``.mhub`` files already in the store directory (also in ``module_sdk.zip``); a
  native module gets no folder unless you write the entry yourself
* ``tools/build_firmware.py … modules`` — compile every in-tree native ``.so`` and
  wrap it; nothing is published to ``Stores/``
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
     meta.json            # optional ui + capabilities

``module.json`` (ELF)
----------------------

.. code-block:: JSON

   {
     "id": "my_native",
     "name": "Example native adapter",
     "version": "0.1.0",
     "enabled": true,
     "adapter": "my_native",
     "binary": "my_native.so",
     "requires": [],
     "abi_version": 1,
     "core_api_min": 1,
     "core_api_max": 1
   }

Set ``requires`` the same way as for JS: list every Host capability token the
native adapter needs (``WiFi``, ``WiFi_SoftAP``, ``Zigbee``, ``BLE``, ``BLE_mesh``, ``Thread_Matter``,
``Matter``, ``MQTT_Broker``, ``Modbus``), or ``[]`` when it needs none. ``BLE``
and ``BLE_mesh`` both follow the ``ble_mesh`` adapter. Pack copies the list
onto ``catalog.json`` for ``caps=`` filtering.

``ui`` and ``capabilities`` now live in ``meta.json`` (a sibling file); module.json
keeps only runtime/environment info.

``binary`` is the filename **inside** the unpacked module directory on device
(for example ``/store/modules/my_native/my_native.so``).

``module_entry.c`` pattern
--------------------------

.. code-block:: c

   #include "minihub_plugin_sdk.h"

   extern const minihub_adapter_t my_adapter;

   /* The vtable belongs to Host and is only valid while the module is loaded:
    * keep it in a file static and call through it from init / start / poll. */
   static const minihub_module_host_api_t *s_host;

   esp_err_t minihub_module_entry(
       const minihub_module_host_api_t *host,
       const cJSON *config,
       const minihub_adapter_t **out_adapter)
   {
       if (!out_adapter) {
           return ESP_ERR_INVALID_ARG;
       }
       s_host = host;   /* save vtable for points/commands */
       (void)config;
       *out_adapter = &my_adapter;
       return ESP_OK;
   }

Optional RPC (``POST /api/modules/ID/rpc``):

.. code-block:: c

   esp_err_t minihub_module_rpc(const char *method, const cJSON *params, cJSON **out);

Host vtable (``minihub_module_host_api_t``)
-------------------------------------------

Prefer the table handed to ``minihub_module_entry`` for Core services: the vtable
is the versioned contract. Symbols on the export allowlist may be linked directly;
only symbols outside the allowlist fail to link. The table covers:

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

A module runs inside a process it does not control, so it may call only what the
Host publishes. Every undefined symbol in the ``.so`` has to resolve against the
Host exports listed in ``docs/module_sdk/host_export_allowlist.json``, and the
Host refuses to load the image when one does not. Declarations come from
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

If ``my_native`` is in the store folder because you placed it there yourself
(step 4), open ``Store`` in the Web UI at ``http://HUB/``, refresh the catalog and
install it: the install reloads the Hub as it goes (send ``"reload": false`` to
skip that). A module that comes in disabled is switched on with **Enable**, which
reloads the same way. Factory ``store_preinstall`` modules are already loaded at
boot, so there the console only has to show the card.

A module that crashes while loading would otherwise crash the Hub on every
restart, so Host writes ``/store/modules/my_native/.load_guard`` = ``loading``
and skips that module on the next boot. Clear it once the bug is fixed:

.. code-block:: bash

   export HUB=<hub-ip>          # from the eth got IP log line; log in once with -c/-b
   curl -s -b /tmp/mh -X POST http://$HUB/api/modules \
     -H 'Content-Type: application/json' \
     -d '{"id":"my_native","clear_load_guard":true,"reload":true}'

Common failures:

* ``abi_version`` mismatch — align with ``MINIHUB_MODULE_ABI_VERSION`` in header
* Relocation / undefined symbol — check allowlist; do not call ``esp_wifi_*`` from module
* ``binary_size`` mismatch after OTA — repack ``.mhub`` after rebuild

Device types, ``devices.json``, and widgets: :doc:`../type_catalog`
(scaffold with ``tools/new_store_module.py --tier l3``).
Point identity (manufacturer, category, OTA, ``owner``) and adapters JSON:
:doc:`../data_model`.

Header reference: :doc:`../api/plugin_host`, :doc:`../api/module_abi`,
:doc:`../api/minihub_core`, :doc:`../api/minihub_adapter`.

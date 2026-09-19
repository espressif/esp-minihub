L3 Host export SDK
==================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

Standalone headers for Store **L3** plugins that compile **without** ESP-IDF or
the MiniHub source tree. Ship ``docs/module_sdk/include/`` plus a matching cross
compiler (ESP32-S31: ``riscv32-esp-elf-gcc``).

Download (HTML): :download:`module_sdk.zip <../../_static/module_sdk.zip>` —
``include/`` headers, ``host_export_allowlist.json``,
``minihub_plugin_standalone.cmake``, ``pack_store_module.py`` (and
``store_paths.py``, which it imports), ``sync_store_catalog.py`` (rewrite
``catalog.json`` from deployed ``.mhub`` files), and README. Unzip and pass
``-I module_sdk/include``. No MiniHub clone required.

.. code-block:: c

   #include "minihub_plugin_sdk.h"

Add ``-I`` to ``docs/module_sdk/include``. libc (``malloc``, ``memcpy``,
``fopen``, ``time``, ``close``, …) comes from the toolchain. Host-exported
symbols are declared in category headers under ``minihub_plugin_sdk/``.

Runtime binding is still the allowlist: :ref:`l3-host-export-allowlist`.

Example compile (do not link the Host ELF):

.. code-block:: bash

   riscv32-esp-elf-gcc -std=gnu17 -fPIC -shared -nostdlib -nostartfiles \
     -fdata-sections -ffunction-sections -fvisibility=hidden \
     -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4 \
     -Wl,--gc-sections -Wl,--allow-shlib-undefined \
     -I docs/module_sdk/include \
     -o my_native.so module_entry.c my_native_adapter.c

In-tree plugins built with ``idf.py`` include only the MiniHub/IDF headers they
use. Do not mix this SDK with IDF headers in the same translation unit.

Header layout
-------------

.. code-block:: text

   docs/module_sdk/include/
     minihub_plugin_sdk.h          # umbrella
     minihub_plugin_sdk/
       esp_idf.h                   # err, log, HTTP, NVS, heap, timer
       lwip.h
       cjson.h
       mbedtls.h
       psa.h
       freertos.h
       minihub.h                   # points, ABI, worker, net, BLE GATT

Category APIs:

* :doc:`plugin_sdk_minihub`
* :doc:`plugin_sdk_esp_idf`
* :doc:`plugin_sdk_cjson`
* :doc:`plugin_sdk_mbedtls`
* :doc:`plugin_sdk_psa`
* :doc:`plugin_sdk_lwip`
* :doc:`plugin_sdk_freertos`

Host ABI check
--------------

When ``CONFIG_MINIHUB_DYNAMIC_MODULES`` is enabled, Host firmware build runs
``tools/check_plugin_sdk.py``:

1. **Coverage** — allowlisted non-libc symbols appear in the SDK headers
2. **Layout** — ``sizeof`` of critical structs must match Host (same cross-compiler)
3. **Prototypes** — SDK declarations recompiled against Host headers

.. code-block:: bash

   python3 tools/check_plugin_sdk.py coverage
   python3 tools/check_plugin_sdk.py emit-check \
     --cc riscv32-esp-elf-gcc \
     --sizes-out /tmp/plugin_sdk_sizes.h \
     --check-out /tmp/plugin_sdk_abi_check.c

Implementation: :doc:`../plugins/develop_l3`.

Module ABI (L3 native plugins)
==============================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

Store **L3** native modules (``module.json`` + MHMD ``.so``) interact with Host through
a binary ABI. Modules export ``minihub_module_entry``, retain the
``minihub_module_host_api_t`` vtable, and return ``minihub_adapter_t``. L2 JS:
:doc:`js_host`. REST install/RPC: :doc:`plugin_rest`.

Runtime constraints (summary):

* Same IDF target as Host (e.g. esp32s31)
* ``module.json`` ``abi_version`` and ``core_api_min`` / ``core_api_max`` must match Host
* Undefined symbols must be a subset of the Host export allowlist
  (see :ref:`l3-host-export-allowlist`). **Declarations:**
  ``#include "minihub_plugin_sdk.h"`` (standalone SDK) — :doc:`plugin_host`
* Extend ``minihub_module_host_api_t`` only at the **tail**; check ``struct_size`` before calls
* A function-pointer ``typedef`` used as a vtable slot has the same layout as
  the pointer written inline. Do not bump ``abi_version`` for that rename.
  Bump it when slot order, signatures, or the tail change.

API Reference
-------------

.. include-build-file:: inc/minihub_module_abi.inc

.. _l3-host-export-allowlist:

Symbol allowlist
----------------

L3 ``.so`` must not link the Host application. Every undefined ELF symbol must
appear in the list below (same file as ``docs/module_sdk/host_export_allowlist.json``).
Host resolves them with ``esp_elf_register_symbol``. Addresses are firmware-local —
do not hardcode.

**Included (by prefix / area)**

* Compiler helpers — ``__divdi3``, ``__eqdf2``, ``__extendsfdf2``, …
* libc — heap, strings, stdio, ``time``, ``open`` / ``read`` / ``write`` / ``close``
* cJSON — ``cJSON_*``
* ESP-IDF — ``esp_http_client_*``, ``esp_log*``, ``esp_timer_get_time``,
  ``esp_crt_bundle_attach``, ``esp_err_to_name``, ``esp_random`` / ``esp_fill_random``
* lwIP — ``lwip_socket``, ``lwip_sendto`` / ``lwip_recvfrom``, ``lwip_inet_*``
* Crypto — ``mbedtls_*``, ``psa_*``
* NVS — ``nvs_*``
* MiniHub — ``minihub_point_*``, ``minihub_config_*``, ``minihub_http_*``,
  ``minihub_net_dns_probe`` / ``minihub_net_has_ip``, ``minihub_hub_reload``,
  ``minihub_module_*``, ``minihub_ble_gatt_*`` (GATT client, not BLE Mesh radio)
* FreeRTOS — ``vTaskDelay``, ``xTaskCreatePinnedToCore``, queue / mutex helpers
* Heap query — ``heap_caps_get_free_size``

**Excluded:** Wi-Fi / Zigbee / BLE Mesh / Matter / OpenThread **radio** APIs;
``httpd`` URI registration.

Regenerate after a Host rebuild:

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

Link flags: ``cmake/minihub_module_so.cmake``.

Full snapshot (keep this JSON as the source of truth):

.. literalinclude:: ../../module_sdk/host_export_allowlist.json
   :language: json

C prototypes: :doc:`plugin_host` (do not re-declare these symbols).
Implementation guide: :doc:`../plugins/develop_l3`.

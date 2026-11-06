Module ABI (native ELF plugins)
================================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

A native module is loaded into a Hub that is already running, so the two sides
have to agree on a binary contract before either can call the other. Store **ELF**
modules (``module.json`` plus an MHMD ``.so``) meet Host through that ABI: the
module exports ``minihub_module_entry``, keeps the ``minihub_module_host_api_t``
vtable it is handed, and returns a ``minihub_adapter_t``. JS is
:doc:`js_host`. REST install/RPC: :doc:`plugin_rest`.

The contract holds only when every one of the following is true:

* Same IDF target as Host (e.g. esp32s31)
* ``abi_version`` / ``core_api_min`` / ``core_api_max`` in ``module.json`` must
  **bracket** the Host — and they are all optional: an omitted ``abi_version`` or
  ``core_api_min`` defaults to the Host's own value, and ``core_api_max`` defaults
  to 0, meaning no upper bound. So a package that leaves them out loads against
  any Host; writing them is what makes it refuse a mismatched one
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

ELF ``.so`` must not link the Host application. Every undefined ELF symbol must
appear in the list below (same file as ``docs/module_sdk/host_export_allowlist.json``).
Host resolves them with ``esp_elf_register_symbol``. Addresses are firmware-local —
do not hardcode.

**Included (by prefix / area)**

* Compiler helpers — ``__divdi3``, ``__eqdf2``, ``__extendsfdf2``, …
* libc — heap (``malloc`` / ``calloc`` / ``realloc`` / ``free``), strings
  (``strcpy`` / ``strlen`` / ``strcmp`` / ``strcasecmp`` …), ``time``, and
  ``snprintf`` / ``vsnprintf`` / ``sscanf``. **There is no ``printf`` family, no
  ``gmtime`` / ``localtime``, and no POSIX ``open`` / ``read`` / ``write``** —
  file I/O is stdio and directory
  calls only: ``fopen`` / ``fclose`` / ``fread`` / ``fwrite`` / ``fseek`` /
  ``ftell`` / ``rewind`` / ``close`` / ``mkdir`` / ``rmdir`` / ``unlink`` / ``stat``
* cJSON — ``cJSON_*``
* ESP-IDF — ``esp_http_client_*``, ``esp_timer_get_time``,
  ``esp_crt_bundle_attach``, ``esp_err_to_name``, ``esp_random`` / ``esp_fill_random``,
  and for logging just ``esp_log`` and ``esp_log_timestamp``: the ``ESP_LOGx``
  macros expand to ``esp_log_write``, which is **not** exported, so use the SDK
  header (it re-maps ``ESP_LOGI`` onto ``esp_log``) rather than an IDF ``esp_log.h``
* lwIP — ``lwip_socket``, ``lwip_sendto`` / ``lwip_recvfrom``, ``lwip_setsockopt``,
  ``lwip_inet_pton`` / ``lwip_inet_ntop``
* Crypto — ``mbedtls_*`` (base64, message digest, HMAC), ``psa_*``
* NVS — ``nvs_*``
* MiniHub — point registry and state, by name: ``minihub_point_register``,
  ``minihub_point_set``, ``minihub_point_set_online``, ``minihub_point_get``,
  ``minihub_point_get_snapshot``, ``minihub_point_merge_params``,
  ``minihub_point_merge_props``, ``minihub_point_list_by_adapter``,
  ``minihub_point_list_free``, ``minihub_point_unregister_by_adapter``.
  ``minihub_point_unregister`` is **not** exported, and neither is
  ``minihub_command_register_adapter`` or ``minihub_adapter_register`` — those go
  through the vtable. Also ``minihub_command_invoke``, ``minihub_config_*``,
  ``minihub_http_*``, ``minihub_net_dns_probe`` / ``minihub_net_has_ip``,
  ``minihub_hub_reload``, ``minihub_module_*``, and the BLE GATT client subset
  ``minihub_ble_gatt_init`` / ``ready`` / ``scan_start`` / ``scan_stop`` /
  ``scan_get`` / ``connect`` / ``disconnect`` / ``is_connected`` /
  ``discover_svc`` / ``find_char`` / ``subscribe`` / ``wait_notify`` / ``write``
  / ``conn_handle`` / ``deinit`` (GATT client, not BLE Mesh radio).
  ``tools/gen_host_export_symbols.py`` derives this list from the same
  ``s_minihub_host_elfsyms[]`` block Host registers at run time, so anything Host
  always exports is on it — a symbol missing from the list is a generator bug to
  report, not something to work around in the plugin
* FreeRTOS — ``vTaskDelay``, ``vTaskDelete``, ``xTaskCreatePinnedToCore``,
  ``xQueueCreateMutex``, ``xQueueGenericSend``, ``xQueueSemaphoreTake``.
  ``xTaskCreate``, ``xQueueSend`` and ``xSemaphoreTake`` are not exported —
  create with the pinned-core form and take the generic names
* Heap query — ``heap_caps_get_free_size``

**Excluded:** Wi-Fi / Zigbee / BLE Mesh / Matter / OpenThread **radio** APIs;
``httpd`` URI registration.

Regenerate after a Host rebuild:

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

Link flags: ``cmake/minihub_module_so.cmake``.

Full snapshot (keep this JSON as the source of truth):

.. literalinclude:: ../../module_sdk/host_export_allowlist.json
   :language: JSON

C prototypes: :doc:`plugin_host` (do not re-declare these symbols).
Implementation guide: :doc:`../plugins/develop_elf`.

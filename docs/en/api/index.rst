API Reference
=============

:link_to_translation:`zh_CN:[中文]`

MiniHub APIs fall into Host development and Store plugin surfaces.

OEM products, LAN integration, automation scripts, and **statically linked**
adapters under ``components/``: REST/MCP against the hub, or implement
``minihub_adapter_t`` in Host firmware.

L2 (JS) modules under ``examples/plugins/`` or
``MINIHUB_PLUGIN_DIRS``: use the
JS ``host`` object, plus REST for install, enable, and RPC.

.. only:: not hide_l3

   L3 (native) modules use the host vtable. Out-of-tree C sources should
   ``#include "minihub_plugin_sdk.h"`` (:doc:`plugin_host`).

.. toctree::
   :maxdepth: 2
   :caption: Host development API

   host_rest
   deploy_index
   mcp
   net_api
   minihub_core
   minihub_adapter

.. toctree::
   :maxdepth: 2
   :caption: Store plugin API

   plugin_rest
   js_host

.. only:: not hide_l3

   .. toctree::
      :maxdepth: 2
      :caption: Store plugin API (native ELF)

      module_abi
      plugin_host
      plugin_sdk_minihub
      plugin_sdk_esp_idf
      plugin_sdk_cjson
      plugin_sdk_mbedtls
      plugin_sdk_psa
      plugin_sdk_lwip
      plugin_sdk_freertos

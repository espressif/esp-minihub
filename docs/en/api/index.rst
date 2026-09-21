API Reference
=============

:link_to_translation:`zh_CN:[中文]`

MiniHub APIs fall into Host development and Store plugin surfaces.

OEM products, LAN integration, automation scripts, and **statically linked**
adapters under ``components/``: REST/MCP against the hub, or implement
``minihub_adapter_t`` in Host firmware.

L2 (JS) and L3 (native) modules under ``components/minihub_plugins/`` or
``examples/plugins/``: use the
host vtable / JS ``host`` object, plus REST for install, enable, and RPC.
Out-of-tree L3 C sources should ``#include "minihub_plugin_sdk.h"``
(:doc:`plugin_host`).

.. toctree::
   :maxdepth: 2
   :caption: Host development API

   host_rest
   mcp
   net_api
   minihub_core
   minihub_adapter

.. toctree::
   :maxdepth: 2
   :caption: Store plugin API

   plugin_rest
   js_host
   module_abi
   plugin_host
   plugin_sdk_minihub
   plugin_sdk_esp_idf
   plugin_sdk_cjson
   plugin_sdk_mbedtls
   plugin_sdk_psa
   plugin_sdk_lwip
   plugin_sdk_freertos

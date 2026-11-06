API Reference
=============

:link_to_translation:`zh_CN:[中文]`

Which API you use depends on where your code runs. This reference splits in two:
one surface for the Hub and the firmware built into it, another for modules
installed onto it.

Code that ships inside the firmware — an OEM product, a LAN integration, an
automation script, or a **statically linked** adapter under ``components/`` —
talks REST or MCP to the hub, or implements ``minihub_adapter_t`` in Host
firmware.

A JS (JavaScript) module under ``examples/plugins/`` or ``MINIHUB_PLUGIN_DIRS`` is not
linked into the firmware. It works through the JS ``host`` object and uses REST for
install, enable and RPC.

.. only:: not hide_l3

   ELF (native) modules use the host vtable. Out-of-tree C sources should
   ``#include "minihub_plugin_sdk.h"`` (:doc:`plugin_host`).

.. toctree::
   :maxdepth: 2
   :caption: Host development API

   host_rest
   deploy_index
   mcp
   errors
   net_api
   minihub_core
   minihub_adapter

.. toctree::
   :maxdepth: 2
   :caption: Store plugin API

   plugin_rest
   js_host
   js_host_core
   js_host_net
   js_host_ble
   js_host_mdns
   js_host_mqtt

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

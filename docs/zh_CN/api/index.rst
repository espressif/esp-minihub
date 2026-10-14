API 参考
========

:link_to_translation:`en:[English]`

MiniHub 的对外接口分为 Host 开发 API 与 Store 插件 API 两类。

面向 OEM 产品、局域网集成、自动化脚本，以及在 ``components/`` 内编写 **静态链接**
适配器的开发者：REST/MCP 操作 Hub，或在 Host 固件中实现 ``minihub_adapter_t`` 。

面向 ``examples/plugins/`` 或 ``MINIHUB_PLUGIN_DIRS`` 下的 L2（JS）
模块作者：通过 JS 对象访问 Core，并用 REST 安装、启用与 RPC。

.. only:: not hide_l3

   L3（原生）模块使用 Host 虚表。仓库外 C 源文件应
   ``#include "minihub_plugin_sdk.h"`` （ :doc:`plugin_host` ）。

.. toctree::
   :maxdepth: 2
   :caption: Host 开发 API

   host_rest
   deploy_index
   mcp
   net_api
   minihub_core
   minihub_adapter

.. toctree::
   :maxdepth: 2
   :caption: Store 插件 API

   plugin_rest
   js_host

.. only:: not hide_l3

   .. toctree::
      :maxdepth: 2
      :caption: Store 插件 API（原生 ELF）

      module_abi
      plugin_host
      plugin_sdk_minihub
      plugin_sdk_esp_idf
      plugin_sdk_cjson
      plugin_sdk_mbedtls
      plugin_sdk_psa
      plugin_sdk_lwip
      plugin_sdk_freertos

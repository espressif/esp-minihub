API 参考
========

:link_to_translation:`en:[English]`

用哪套 API 取决于代码跑在哪里。本参考一分为二：一套给 Hub 和编进 Hub 的固件，
一套给装到 Hub 上的模块。

编进固件的代码 —— OEM 产品、局域网集成、自动化脚本，或 ``components/`` 内
**静态链接** 的适配器 —— 用 REST/MCP 操作 Hub，或在 Host 固件中实现
``minihub_adapter_t``。

``examples/plugins/`` 或 ``MINIHUB_PLUGIN_DIRS`` 下的 L2（JS）模块没有链进固件。
它通过 JS ``host`` 对象工作；安装、启用与 RPC 走 REST。

.. only:: not hide_l3

   L3（原生）模块使用 Host 虚表。仓库外 C 源文件应
   ``#include "minihub_plugin_sdk.h"`` （ :doc:`plugin_host` ）。

.. toctree::
   :maxdepth: 2
   :caption: Host 开发 API

   host_rest
   deploy_index
   mcp
   errors
   net_api
   minihub_core
   minihub_adapter

.. toctree::
   :maxdepth: 2
   :caption: 商店插件 API

   plugin_rest
   js_host

.. only:: not hide_l3

   .. toctree::
      :maxdepth: 2
      :caption: 商店插件 API（原生 ELF）

      module_abi
      plugin_host
      plugin_sdk_minihub
      plugin_sdk_esp_idf
      plugin_sdk_cjson
      plugin_sdk_mbedtls
      plugin_sdk_psa
      plugin_sdk_lwip
      plugin_sdk_freertos

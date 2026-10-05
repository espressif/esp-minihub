开发 L3（原生）模块
===================

:link_to_translation:`en:[English]`

L3 模块在外层 ``.mhub`` zip 内携带 **原生 ELF** ``.so`` 。加载器要求 ``.so``
带 **MHMD** 头（见 ``tools/pack_store_module.py mhmd`` ）。Host ``dlopen`` 后调用
``minihub_module_entry`` 。

端到端流程
----------

1. **脚手架** — ``module.json`` 、源码、可选 ``ui.json``
2. **编译** ``.so`` — 下面两种方式选一
3. **打包** — ``python3 tools/pack_store_module.py pack ID``
4. **目录** — ``.mhub`` 已在 store 目录后，重写 ``catalog.json`` （Hub 读这份
   文件列模块，不会自己扫 zip）：
   ``python3 tools/sync_store_catalog.py --from-store``
5. **设备安装** — Store UI、``POST /api/store/install`` 或工厂 ``store_preinstall``
6. **重载** — ``POST /api/hub/reload`` ；检查串口与 ``GET /api/modules``

两种编译方式
------------

仓库外单独编
^^^^^^^^^^^^^^

不需要 ESP-IDF、也不需要 MiniHub 源码树。用 Plugin SDK 头
（ ``#include "minihub_plugin_sdk.h"`` ）和与 Host 同架构的交叉编译器
（ESP32-S31： ``riscv32-esp-elf-gcc`` ）。同一翻译单元不要把 SDK 头与 IDF 头混用。
完整头文件说明：:doc:`../api/plugin_host`
（:download:`module_sdk.zip <../../_static/module_sdk.zip>`）。

.. code-block:: bash

   # 1. 编译原始 ELF（中间文件）
   mkdir -p build/lib
   riscv32-esp-elf-gcc -std=gnu17 -shared -fPIC \
     -nostdlib -nostartfiles \
     -fdata-sections -ffunction-sections -fvisibility=hidden \
     -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4 \
     -Wl,--gc-sections -Wl,--allow-shlib-undefined \
     -Wl,-u,minihub_module_entry \
     -I /path/to/module_sdk/include \
     -o build/lib/my_native.so module_entry.c my_native_adapter.c

   # 2. 加 MHMD 头 — 最终 .so 留在本插件的 build/
   #    pack_store_module.py 与 store_paths.py 在 module_sdk.zip 里
   python3 /path/to/module_sdk/pack_store_module.py mhmd \
     build/lib/my_native.so build/my_native.so

   # 3. 打 .mhub（必需；Hub 安装的是 zip，不是裸 .so）
   export MINIHUB_PLUGIN_DIRS="$PWD"
   python3 /path/to/module_sdk/pack_store_module.py pack my_native

   # 4. 按 store 里每个 Stores/ESP32S31 或 Stores/Common 的 ID.mhub 重写 catalog.json（不需要插件源码）。
   #    sync_store_catalog.py 与 pack_store_module.py 一起在 module_sdk.zip 里。
   python3 /path/to/module_sdk/sync_store_catalog.py --from-store \
     --deploy ~/workspace/minihub_store

``-shared -fPIC`` 以及 ``-Wl,-z,max-page-size=4`` 在 ESP32-S31 上 ``dlopen`` 必需。
若同时使用 ``-fvisibility=hidden`` 与 ``--gc-sections`` ，还需
``-Wl,-u,minihub_module_entry`` （且 ``module_entry.c`` 使用默认可见性），否则入口会被删掉。
不要链接 Host ELF。 ``.so`` 中未定义符号须落在
``docs/module_sdk/host_export_allowlist.json`` 内，运行时由 Hub 解析。
函数声明来自 ``minihub_plugin_sdk.h`` ，不要手写 ``extern`` 。
**排除：** 射频栈 API 与 httpd URI 注册。详见
:ref:`l3-host-export-allowlist` 。

独立 CMake 片段（可复制到你的工程）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

多源文件或有 ``devices.json`` 时，优先用 CMake 而不是单行 ``gcc`` 。从
:download:`module_sdk.zip <../../_static/module_sdk.zip>` 取出
``minihub_plugin_standalone.cmake``，放进插件目录里（与 ``CMakeLists.txt`` 同级，
或插件内的 ``cmake/`` ；不是插件文件夹的上一级）。默认 ``MINIHUB_MODULE_ID``
为插件目录名，``MINIHUB_MODULE_SRCS`` 为该目录下全部 ``*.c`` 与 ``*.cpp``
（不含子目录）。需要时在 ``include()`` 前覆盖。第三方再设
``MINIHUB_SDK_INCLUDE`` 。然后：

.. code-block:: bash

   cmake -S . -B build \
     -DCMAKE_C_COMPILER=riscv32-esp-elf-gcc \
     -DCMAKE_SYSTEM_NAME=Generic \
     -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
   cmake --build build

第一条只做 configure（第一次必须跑；改了 ``CMakeLists.txt`` 再跑）。第二条才编译。
第一次配好之后，改源码只需 ``cmake --build build``。MHMD 文件是 ``build/<id>.so``。

再打成 ``.mhub`` （当前目录为插件目录；打包脚本在 MiniHub 仓库里）：

.. code-block:: bash

   export MINIHUB_PLUGIN_DIRS="$PWD"
   python3 /path/to/module_sdk/pack_store_module.py pack "$(basename "$PWD")"
   python3 /path/to/module_sdk/sync_store_catalog.py --from-store \
     --deploy ~/workspace/minihub_store

``-B build`` 放在插件目录内。``build_plugin_standalone.py``（本仓库
``tools/`` 或 ``docs/module_sdk/``；``module_sdk.zip`` 里与 cmake 片段同目录）
会做 configure、编译，并可加 ``--pack``。Host 树内编译仍写到
``application/minihub/build/<id>.so``。

``examples/plugins/`` 演示（双路径）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``color_bulb`` 、``meter_plug`` 、``zigbee_ota`` 、``ble_mesh_ota`` 、``matter_ota``
均用 ``if(ESP_PLATFORM)`` / 独立 ``include()`` 上述片段。不依赖 IDF：

.. code-block:: bash

   python3 tools/build_plugin_standalone.py color_bulb
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack color_bulb
   python3 tools/sync_store_catalog.py --from-store

仓库内跟 Host 一起编
^^^^^^^^^^^^^^^^^^^^

源码放 ``examples/plugins/ID/`` （演示）或 ``MINIHUB_PLUGIN_DIRS``
列出的树（CMake **之前** export），用
``idf_component_register`` ；
``REQUIRES`` 只加实际用到的公开库。使能 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 时加
``-fPIC`` 。固件编译经 ``cmake/minihub_module_so.cmake`` 生成
``application/minihub/build/modules/libadapter_ID.so`` ，成功后会自动 MHMD 并 pack。

.. code-block:: bash

   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # 仅演示
   ./tools/build_firmware.py -app minihub build

也可在已有 ``.so`` 后只再打包： ``python3 tools/pack_store_module.py pack ID`` 。

L3 常用脚本
^^^^^^^^^^^

* ``tools/new_store_module.py --tier l3 --id ID`` — 脚手架
* ``tools/generate_devices_property_from_json.py`` — ``devices.json`` →
  ``device_desc.gen.h`` （如有）
* ``tools/build_plugin_standalone.py ID`` — 演示插件无 IDF 的 CMake 编译
* ``tools/pack_store_module.py mhmd`` / ``pack`` — MHMD 与 ``.mhub``
* ``tools/sync_store_catalog.py --from-store`` — ``.mhub`` 已在 store 目录后
  重写 ``catalog.json`` （也在 ``module_sdk.zip`` 里）
* ``tools/build_firmware.py … modules`` — 编译全部树内原生 ``.so``；``pack`` 打 ``.mhub``
* ``tools/check_plugin_sdk.py`` — Host 侧 SDK 与白名单检查
* IDE MCP：``python3 tools/minihub_plugin_mcp.py`` （脚手架 / 校验 / 打包；
  不交叉编译 ``.so`` ）

另见 :doc:`pack_and_catalog` 与 :doc:`overview` 。

目录结构（脚手架 ``my_native`` ）
---------------------------------

.. code-block:: text

   examples/plugins/my_native/
     module.json
     module_entry.c       # 导出 minihub_module_entry（可选 minihub_module_rpc）
     my_native_adapter.c  # minihub_adapter_t 实现
     CMakeLists.txt
     ui.json              # 可选 Web UI

``module.json`` （L3）
----------------------

.. code-block:: json

   {
     "id": "my_native",
     "name": "示例原生适配器",
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

``binary`` 是设备解压目录内的文件名（例如 ``/store/modules/my_native/my_native.so`` ）。

``module_entry.c`` 模式
-----------------------

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
       my_module_set_host(host);
       (void)config;
       *out_adapter = &my_adapter;
       return ESP_OK;
   }

可选 RPC（``POST /api/modules/ID/rpc`` ）：

.. code-block:: c

   esp_err_t minihub_module_rpc(const char *method, const cJSON *params, cJSON **out);

``CMakeLists.txt`` 要点
-----------------------

仓库内：``REQUIRES`` 只加实际用到的公开库，头文件自己 include；不得链接完整 Host
应用。

Host 虚表（``minihub_module_host_api_t`` ）
-------------------------------------------

模块只能通过 ``minihub_module_entry`` 传入的虚表调用 Host：

* ``point_register`` / ``point_unregister`` / ``point_get`` / ``point_set`` —
  一台设备一条点；实时参数放 ``props`` （``params.名称``）。更新遥测或扩展时
  优先用白名单中的 ``minihub_point_merge_params`` /
  ``minihub_point_merge_props``，以免冲掉顶层键（``ota``、``ieee`` 等）
* ``command_register_adapter``
* ``event_publish`` / ``event_subscribe`` — 至少订阅 ``point.updated`` /
  ``point.registered``，在同一 Host ``point_id`` 上补充字段（不要为同一盏灯
  再注册第二个点）
* ``config_get_adapters`` / ``config_save_adapters``
* ``hub_reload`` / ``adapter_soft_reload``

头文件：``#include "minihub_plugin_sdk.h"`` — :doc:`../api/plugin_host`。
虚表类型在该 SDK 头中（亦见 :doc:`../api/module_abi`）。

符号白名单
----------

``.so`` 中未定义符号须落在 ``docs/module_sdk/host_export_allowlist.json`` 内。
函数声明来自 ``minihub_plugin_sdk.h`` ，不要手写 ``extern`` 。
**排除：** 射频栈 API 与 httpd URI 注册。完整列表见
:ref:`l3-host-export-allowlist` 。

Host 导出变更后：

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

Host 编译还会运行 ``tools/check_plugin_sdk.py``，防止 ``minihub_plugin_sdk.h``
与 IDF/MiniHub 布局或白名单脱节。

安装与调试
----------

.. code-block:: bash

   curl -X POST http://$HUB/api/store/install -H 'Content-Type: application/json' \
     -d '{"kind":"module","id":"my_native"}'
   curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
     -d '{"id":"my_native","enabled":true,"reload":true}'

加载崩溃时 Host 写入 ``/store/modules/my_native/.load_guard`` = ``loading``，下次启动
会跳过。清除并重试：

.. code-block:: bash

   curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
     -d '{"id":"my_native","clear_load_guard":true,"reload":true}'

常见错误：

* ``abi_version`` 不匹配 — 对齐 ``MINIHUB_MODULE_ABI_VERSION``
* 重定位 / 未定义符号 — 查白名单；模块内勿调用 ``esp_wifi_*`` 等
* ``binary_size`` 与 OTA 后不一致 — 重新编译并打包 ``.mhub``

设备类型、``devices.json`` 与控件：:doc:`../type_catalog` （演示
``examples/plugins/color_bulb``、``examples/plugins/meter_plug``）。
点位身份（厂商、品类、OTA、``owner``）与 adapters JSON：:doc:`../data_model`。

头文件说明：:doc:`../api/plugin_host`、:doc:`../api/module_abi`、
:doc:`../api/minihub_core`、:doc:`../api/minihub_adapter`。

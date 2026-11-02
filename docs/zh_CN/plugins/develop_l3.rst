开发 L3（原生）模块
===================

:link_to_translation:`en:[English]`

L3 模块在外层 ``.mhub`` zip 内携带 **原生 ELF** ``.so``。Host 对该镜像 ``dlopen``
后调用 ``minihub_module_entry``，所以这个二进制必须是为将要加载它的那台 Host 编的
—— 安装时没有任何东西会替你做 ABI 转换。打包前用 **MHMD** 头封装 ``.so``
（``tools/pack_store_module.py mhmd`` ）；加载器靠这个头识别镜像。

端到端流程
----------

打包原生模块比打包 JS 模块多两步：``.so`` 要先编出来、套好头，才有东西可 zip。

1. **脚手架** — ``module.json``、源码、可选 ``meta.json``
2. **编译** ``.so`` — 下面两种方式选一
3. **打包** — 原生包没有这条路： ``tools/pack_store_module.py pack ID`` 一遇
   ``runtime: native`` 就 skip ，连 ``.mhub`` 都不产出（ ``pack_store_module.py:236-239`` ）。
   MHMD 封装本身用 ``tools/pack_store_module.py mhmd in.so out.so`` 。
4. **上架** — ``tools/sync_store_catalog.py`` 不给原生模块分配 ``Stores/`` 目录，
   从源码重打的 ``all`` 还会删掉早先留下的那种目录（ ``--from-store`` 不动它，
   所以手写的那份能留着）。要走商店安装，得自己把 ``ID.mhub`` 放进
   ``Stores/Common/ID/`` 并在同目录手写 ``catalog.json`` —— 设备侧是认原生包的，
   只是工具不替你发布。
5. **装到设备** — 量产走工厂 ``store_preinstall`` ： ``module.json`` 与 MHMD
   ``.so`` 被 staged 进 ``modstore`` 镜像，开机 ``scan_manifests`` 扫到就加载，
   不必点任何重载。自己往 ``/store/modules/ID/`` 放文件时（ ``PUT /api/dev/fs/``
   写不到 ``/store`` ，只能刷 ``modstore`` 或设备侧 shell ），没有任何东西在监听
   那个目录，必须 ``POST /api/hub/reload`` 才会被重新扫到；从商店装则装完自动重载。
6. **验证** — 串口里的 ``module 'ID' load ok`` ，以及 ``GET /api/modules`` 的
   ``loaded`` / ``load_blocked`` / ``binary_stale`` / ``abi_version`` 。光放一个
   ``.so`` 是不够的：目录里没有 ``module.json`` 时扫描直接跳过，而且不打日志。

两种编译方式
------------

选哪一种，取决于你手上有没有 MiniHub 源码树。

仓库外单独编
^^^^^^^^^^^^^^

这条路径不需要 ESP-IDF，也不需要 MiniHub 源码树。开发者只用 Plugin SDK 头
（ ``#include "minihub_plugin_sdk.h"`` ）和与 Host 同架构的交叉编译器
（ESP32-S31： ``riscv32-esp-elf-gcc`` ）就能编。这些头描述的是 Host 导出的样子；
同一翻译单元里再混 IDF 头，同一批符号会得到两套互相矛盾的定义 —— 不要混。
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

   # 3. MHMD 留在本插件的 build/。pack 不创建商店插件目录。
   export MINIHUB_PLUGIN_DIRS="$PWD"
   python3 /path/to/module_sdk/pack_store_module.py pack my_native

``-shared -fPIC`` 以及 ``-Wl,-z,max-page-size=4`` 在 ESP32-S31 上 ``dlopen`` 必需。
若同时使用 ``-fvisibility=hidden`` 与 ``--gc-sections``，还需
``-Wl,-u,minihub_module_entry`` （且 ``module_entry.c`` 使用默认可见性），否则入口会被删掉。
不要链接 Host ELF。 ``.so`` 中未定义符号须落在
``docs/module_sdk/host_export_allowlist.json`` 内，运行时由 Hub 解析。
函数声明来自 ``minihub_plugin_sdk.h``，不要手写 ``extern``。
**排除：** 射频栈 API 与 httpd URI 注册。详见
:ref:`l3-host-export-allowlist`。

独立 CMake 片段（可复制到你的工程）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

多源文件或有 ``devices.json`` 时，优先用 CMake 而不是单行 ``gcc``。从
:download:`module_sdk.zip <../../_static/module_sdk.zip>` 取出
``minihub_plugin_standalone.cmake``，放进插件目录里（与 ``CMakeLists.txt`` 同级，
或插件内的 ``cmake/``；不是插件文件夹的上一级）。默认 ``MINIHUB_MODULE_ID``
为插件目录名，``MINIHUB_MODULE_SRCS`` 为该目录下全部 ``*.c`` 与 ``*.cpp``
（不含子目录）。需要时在 ``include()`` 前覆盖。第三方再设
``MINIHUB_SDK_INCLUDE``。然后：

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

``-B build`` 放在插件目录内。``build_plugin_standalone.py`` （本仓库
``tools/`` 或 ``docs/module_sdk/``；``module_sdk.zip`` 里与 cmake 片段同目录）
会做 configure、编译，并可加 ``--pack``。Host 树内编译仍写到
``application/minihub/build/<id>.so``。

脚手架出的 L3 插件（双路径）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

用 ``if(ESP_PLATFORM)`` / 独立 ``include()`` 切分上述片段，同一份源码两边都能编。
不依赖 IDF：

.. code-block:: bash

   python3 tools/new_store_module.py --tier l3 --id my_native
   python3 tools/build_plugin_standalone.py my_native
   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
   python3 tools/pack_store_module.py pack my_native
   python3 tools/sync_store_catalog.py --from-store

仓库内跟 Host 一起编
^^^^^^^^^^^^^^^^^^^^

源码放 ``examples/plugins/ID/`` （演示）或 ``MINIHUB_PLUGIN_DIRS``
列出的树（CMake **之前** export），用
``idf_component_register``；
``REQUIRES`` 只加实际用到的公开库。使能 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 时加
``-fPIC``。``cmake/minihub_module_so.cmake`` 在编译模块目标时生成
``application/minihub/build/modules/libadapter_ID.so``，再做 MHMD 和 pack。
Host 固件 ``build`` 不会做这件事。

.. code-block:: bash

   export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # 仅演示
   ./tools/build_firmware.py -app minihub modules

已有 MHMD ``.so`` 时不必重编： ``python3 tools/pack_store_module.py mhmd in.so out.so``
只套头。 ``pack`` 只发布 L1 与 L2 ，原生包按上面第 3-5 步自己放进商店目录，或者走工厂
镜像。

L3 常用脚本
^^^^^^^^^^^

* ``tools/new_store_module.py --tier l3 --id ID`` — 脚手架
* ``tools/generate_devices_property_from_json.py`` — ``devices.json`` →
  ``device_desc.gen.h`` （如有）
* ``tools/build_plugin_standalone.py ID`` — 演示插件无 IDF 的 CMake 编译
* ``tools/pack_store_module.py mhmd`` — MHMD 封装 （ ``pack`` 遇原生模块直接 skip ，
  不产 ``.mhub`` ）
* ``tools/sync_store_catalog.py --from-store`` — 为 store 目录里已有的 ``.mhub``
  重写 ``catalog.json`` （也在 ``module_sdk.zip`` 里）；原生模块除非自己写条目，
  否则拿不到目录
* ``tools/build_firmware.py … modules`` — 编译全部树内原生 ``.so`` 并套 MHMD ；
  不发布到 ``Stores/``
* ``tools/check_plugin_sdk.py`` — Host 侧 SDK 与白名单检查
* IDE MCP：``python3 tools/minihub_plugin_mcp.py`` （脚手架 / 校验 / 打包；
  不交叉编译 ``.so`` ）

另见 :doc:`pack_and_catalog` 与 :doc:`overview`。

目录结构（脚手架 ``my_native`` ）
---------------------------------

.. code-block:: text

   examples/plugins/my_native/
     module.json
     module_entry.c       # 导出 minihub_module_entry（可选 minihub_module_rpc）
     my_native_adapter.c  # minihub_adapter_t 实现
     CMakeLists.txt
     meta.json            # 可选 ui + capabilities

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
     "abi_version": 1,
     "core_api_min": 1,
     "core_api_max": 1
   }

``ui`` 与 ``capabilities`` 现放在同目录的 ``meta.json`` 里；module.json 只保留运行环境信息。

``binary`` 是设备解压目录内的文件名（例如 ``/store/modules/my_native/my_native.so`` ）。

``module_entry.c`` 模式
-----------------------

.. code-block:: c

   #include "minihub_plugin_sdk.h"

   extern const minihub_adapter_t my_adapter;

   /* 虚表归 Host 所有，只在模块被加载期间有效：存进文件级 static，
      在 init / start / poll 里通过它调用。 */
   static const minihub_module_host_api_t *s_host;

   esp_err_t minihub_module_entry(
       const minihub_module_host_api_t *host,
       const cJSON *config,
       const minihub_adapter_t **out_adapter)
   {
       if (!out_adapter) {
           return ESP_ERR_INVALID_ARG;
       }
       s_host = host;
       (void)config;
       *out_adapter = &my_adapter;
       return ESP_OK;
   }

可选 RPC（``POST /api/modules/ID/rpc`` ）：

.. code-block:: c

   esp_err_t minihub_module_rpc(const char *method, const cJSON *params, cJSON **out);

Host 虚表（``minihub_module_host_api_t`` ）
-------------------------------------------

Core 服务优先用 ``minihub_module_entry`` 传入的虚表：虚表是版本化契约。
白名单内的 Host 符号可以直接链接，只有白名单之外的符号才会链接失败。
虚表覆盖：

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

模块跑在一个它管不了的进程里，所以只能调用 Host 公开的东西。``.so`` 中每个未定义符号
都要落在 ``docs/module_sdk/host_export_allowlist.json`` 内，落不下的话 Host 直接拒绝加载。
函数声明来自 ``minihub_plugin_sdk.h``，不要手写 ``extern``。
**排除：** 射频栈 API 与 httpd URI 注册。完整列表见
:ref:`l3-host-export-allowlist`。

Host 导出变更后：

.. code-block:: bash

   python3 tools/gen_host_export_symbols.py

Host 编译还会运行 ``tools/check_plugin_sdk.py``，防止 ``minihub_plugin_sdk.h``
与 IDF/MiniHub 布局或白名单脱节。

安装与调试
----------

如果 ``my_native`` 是按第 4 步自己写进商店目录的，在 Web UI（ ``http://HUB/`` ）里打开
``商店``，刷新目录并安装 —— 装完自动重载（不想重载就在请求里传 ``"reload": false`` ）。
装成停用状态的用 ``启用`` 按钮打开，走的是同一次重载。工厂 ``store_preinstall``
那条路开机就加载好了，界面上只需确认设备卡片。

否则一个加载即崩的模块会让 Hub 每次重启都崩，所以 Host 会写入
``/store/modules/my_native/.load_guard`` = ``loading``，下次启动跳过该模块。
修好后清除并重试：

.. code-block:: bash

   export HUB=<hub-ip>          # 来自 eth got IP 日志；先按 first_plugin 登录一次
   curl -s -b /tmp/mh -X POST http://$HUB/api/modules \
     -H 'Content-Type: application/json' \
     -d '{"id":"my_native","clear_load_guard":true,"reload":true}'

常见错误：

* ``abi_version`` 不匹配 — 对齐 ``MINIHUB_MODULE_ABI_VERSION``
* 重定位 / 未定义符号 — 查白名单；模块内勿调用 ``esp_wifi_*`` 等
* ``binary_size`` 与 OTA 后不一致 — 重新编译并打包 ``.mhub``

设备类型、``devices.json`` 与控件：:doc:`../type_catalog`
（用 ``tools/new_store_module.py --tier l3`` 生成）。
点位身份（厂商、品类、OTA、``owner``）与 adapters JSON：:doc:`../data_model`。

头文件说明：:doc:`../api/plugin_host`、:doc:`../api/module_abi`、
:doc:`../api/minihub_core`、:doc:`../api/minihub_adapter`。

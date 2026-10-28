L3 Host 导出 SDK
================

:link_to_translation:`en:[English]`

概述
----

面向商店 **L3** 插件的独立头文件：可在 **没有** ESP-IDF、也没有 MiniHub 源码树的
情况下编译。交付 ``docs/module_sdk/include/`` 以及与 Host 匹配的交叉编译器
（ESP32-S31：``riscv32-esp-elf-gcc``）。

HTML 下载：:download:`module_sdk.zip <../../_static/module_sdk.zip>` —
含 ``include/``、``templates/``、``host_export_allowlist.json``、
``minihub_plugin_standalone.cmake``、``build_plugin_standalone.py``、
``pack_store_module.py`` （及其依赖 ``store_paths.py``）、
``sync_store_catalog.py`` （按已部署的 ``.mhub`` 重写 ``catalog.json``）与
README。解压后 ``-I module_sdk/include``。不必再克隆 MiniHub。

.. code-block:: c

   #include "minihub_plugin_sdk.h"

把 ``docs/module_sdk/include`` 加到 ``-I``。libc（``malloc``、``memcpy``、``fopen``、
``time``、``close`` …）来自工具链。Host 导出符号声明在 ``minihub_plugin_sdk/``
下的分类头中。

运行时绑定仍以白名单为准：:ref:`l3-host-export-allowlist`。

编译示例（不要链接 Host ELF）：

.. code-block:: bash

   riscv32-esp-elf-gcc -std=gnu17 -fPIC -shared -nostdlib -nostartfiles \
     -fdata-sections -ffunction-sections -fvisibility=hidden \
     -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4 \
     -Wl,--gc-sections -Wl,--allow-shlib-undefined \
     -I docs/module_sdk/include \
     -o my_native.so module_entry.c my_native_adapter.c

仓库内用 ``idf.py`` 编的插件按需 include MiniHub/IDF 头。同一翻译单元不要把 SDK
与 IDF 头混用。

头文件布局
----------

.. code-block:: text

   docs/module_sdk/include/
     minihub_plugin_sdk.h          # 汇总头
     minihub_plugin_sdk/
       esp_idf.h                   # err、log、HTTP、NVS、heap、timer
       lwip.h
       cjson.h
       mbedtls.h
       psa.h
       freertos.h
       minihub.h                   # 点位、ABI、worker、net、BLE GATT

分类 API：

* :doc:`plugin_sdk_minihub`
* :doc:`plugin_sdk_esp_idf`
* :doc:`plugin_sdk_cjson`
* :doc:`plugin_sdk_mbedtls`
* :doc:`plugin_sdk_psa`
* :doc:`plugin_sdk_lwip`
* :doc:`plugin_sdk_freertos`

Host ABI 校验
-------------

启用 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 时，Host 固件编译会运行
``tools/check_plugin_sdk.py``：

1. **覆盖率** — 白名单中非 libc 符号出现在 SDK 头中
2. **布局** — 关键结构体 ``sizeof`` 须与 Host 一致（同一交叉编译器）
3. **原型** — 对着 Host 头重编译 SDK 声明

.. code-block:: bash

   python3 tools/check_plugin_sdk.py coverage
   python3 tools/check_plugin_sdk.py emit-check \
     --cc riscv32-esp-elf-gcc \
     --sizes-out /tmp/plugin_sdk_sizes.h \
     --check-out /tmp/plugin_sdk_abi_check.c

实现指南：:doc:`../plugins/develop_l3`。

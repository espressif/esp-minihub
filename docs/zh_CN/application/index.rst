application 产品
================

:link_to_translation:`en:[English]`

``application/`` 下每个子目录是一份可烧录的 ESP-IDF **产品**。本页是总览；
下方每一章对应一个产品。阅读后再看 :doc:`../host_customization`
（如何复制产品、交付 OEM SKU）。

.. toctree::
   :maxdepth: 1

   minihub

.. rubric:: 产品是什么

MiniHub 把 **产品** 与 **Core** 分开：

* ``application/APP/`` — 实际烧录的 Host：板级、分区表、sdkconfig 组合、出厂
  JSON、``app_main``、产品 Kconfig。目录名即产品名（``APP``）。
* ``components/`` — 共享 Core、协议适配器。Store 源码在
  ``examples/plugins/`` 或 ``MINIHUB_PLUGIN_DIRS``。所有产品都链接 Core
  与适配器，不要拷进 ``application/APP/``。
* 仓库根 ``configs/`` — 共享 manifest 基座（``configs/minihub_manifest.yml`` 里的
  ``_host_base``）以及共享 sdkconfig 片段。

选择产品：``-app APP`` 或 ``MINIHUB_APP=APP`` （默认 ``minihub``）。
封装脚本会套用板级默认值，并在 ``application/APP/`` 下调用 IDF 构建。
见 :doc:`../getting_started`。

.. rubric:: 典型文件

.. code-block:: text

   application/APP/
     manifest.yml
     partitions.csv
     CMakeLists.txt
     configs/
       adapters.json
       sdkconfig/app.yml
     main/
       main.c
       CMakeLists.txt
       Kconfig.projbuild
       adapters_setup.h
       adapters_setup.generated.c   生成文件，勿手改
       idf_component.yml           生成文件，勿手改

``rules.json`` / ``scenes.json`` 可放在产品 ``configs/``，缺失时回退到仓库
``configs/``。

``tools/sync_host_manifest.py`` 把产品 ``application/APP/manifest.yml``
（``extends: _host_base``）与仓库 ``configs/minihub_manifest.yml`` 里的共享基座
``_host_base`` 合并，再写出 IDF 构建要用的生成文件（勿手改）：

* ``application/APP/main/idf_component.yml``
* ``application/APP/main/adapters_setup.generated.c``
* ``cmake/generated/minihub_components.cmake`` （以及同目录若干 ``*.txt``）

``./tools/build_firmware.py`` 编译前会自动跑这一步。

``sdkconfig_features`` 须与 ``configs/sdkconfig/app.yml`` 的 ``features:`` 一致。
Compose：``tools/compose_sdkconfig.py``。协议栈列表：
``cmake/generated/sdkconfig_stacks.gen``。

.. rubric:: 产品与 Core 各放什么

放在 ``application/APP/`` 的：

* 板级、分区、出厂 JSON、sdkconfig 特性
* 产品 ``main/CMakeLists.txt`` 里列出的额外源文件
* 仅该产品的 Kconfig（``main/Kconfig.projbuild``）

留在 ``components/`` （或 Store 插件）的：

* 共享 Hub 服务与适配器
* 厂商云、OEM 设备目录、云 token —— 不要写进公开 ``configs/`` 种子或 Core。
  用 ``.mhub`` 或私有产品树交付。

Web 控制台来自 ``components/minihub_web/web/``，打进
``/data/web/``。默认没有 ``application/APP/web/`` 覆盖层，除非改该
产品的 ``CMakeLists.txt``。见 :doc:`../web_console`。

仓库里若再增加通用产品，在 ``minihub.rst`` 旁增加同级 RST，并写入上方
toctree。私有 OEM 拷贝不必在此建章，按 :doc:`../host_customization` 即可。

minihub
=======

:link_to_translation:`en:[English]`

通用产品：``application/minihub/``。用 ``-app minihub`` 或
``MINIHUB_APP=minihub`` 选择。产品与 Core 的通用规则见 :doc:`index`。

定位
----

默认选项偏以太网（Wi-Fi STA 兜底、USB MSC、RTSP、Modbus 等）。参考板
``esp32_s31_function_coreboard_1``。

::

   ./tools/build_firmware.py -app minihub build

LCD 选项
--------

屏上 UI 由 ``CONFIG_MINIHUB_LCD_UI`` 打开（刷新间隔
``CONFIG_MINIHUB_LCD_UI_REFRESH_MS``；开发期可写 UI JSON 的
``CONFIG_MINIHUB_DEV_FS_UPLOAD``）。该选项 ``depends on !MINIHUB_NET_ETH``。

目录结构
--------

.. code-block:: text

   application/minihub/
     manifest.yml
     partitions.csv
     CMakeLists.txt
     README.md
     configs/
       adapters.json
       sdkconfig/
         app.yml
         feature/*.defaults
         variants/
         local.overrides.example
     main/
       main.c
       CMakeLists.txt
       Kconfig.projbuild
       adapters_setup.h
       adapters_setup.generated.c
       idf_component.yml

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - 路径
     - 作用
   * - ``manifest.yml``
     - 产品配置：板级、分区表、``sdkconfig_features``、``store_preinstall``、
       ``variants``。编译时与仓库 ``configs/minihub_manifest.yml`` 的
       ``_host_base`` 合并，并写出 ``main/idf_component.yml`` 等生成文件
       （见 :doc:`index`）。本表路径都在 ``application/minihub/`` 下，不是仓库根的
       ``configs/`` 。产品目录缺 JSON 时回退到仓库 ``configs/*.json``
       （:doc:`../host_customization`）。
   * - ``partitions.csv``
     - Flash 分区表（``nvs`` / ``ota`` / ``storage`` / ``modstore`` 等）。起点而非
       所有 SKU 规格，见 :doc:`../production`。
   * - ``CMakeLists.txt``
     - IDF 工程入口：把仓库 ``components/`` 、``adapters/``
       加入 ``EXTRA_COMPONENT_DIRS``；Store 插件走
       ``MINIHUB_PLUGIN_DIRS`` （``examples/plugins``），不是 Host 内置树；
       按环境可选 Matter / RainMaker。
   * - ``README.md``
     - 本产品实验室编译备忘（``-app minihub``、LCD 选项）。
   * - ``configs/adapters.json``
     - 出厂适配器配置，打进 LittleFS ``/data/config/``。缺失则回退仓库根
       ``configs/adapters.json``。
   * - ``configs/rules.json`` / ``scenes.json``
     - 通用产品未放这两份文件，构建时用仓库 ``configs/``。OEM 可在产品
       ``configs/`` 覆盖。
   * - ``configs/sdkconfig/app.yml``
     - 本产品 compose 选项（板级 + ``features``）。须与 manifest 的
       ``sdkconfig_features`` 一致。
   * - ``configs/sdkconfig/feature/*.defaults``
     - 仅本产品需要的 Kconfig 片段（USB MSC、RTSP、Modbus 等）。共享片段在仓库
       ``configs/sdkconfig/``。
   * - ``configs/sdkconfig/variants/``
     - 打开 ``CONFIG_MINIHUB_LCD_UI``。
   * - ``configs/sdkconfig/local.overrides.example``
     - 复制为 ``local.overrides`` （git 忽略）写入实验室 Wi-Fi，勿提交。
   * - ``main/main.c``
     - ``app_main``：NVS、网络、Hub、Web。一般不用改；协议栈靠 Kconfig 与生成的
       ``adapters_setup.generated.c``。
   * - ``main/CMakeLists.txt``
     - 产品源文件列表、LittleFS 出厂镜像、Web 预装拷贝。
   * - ``main/Kconfig.projbuild``
     - 产品 Kconfig：Wi-Fi、配网、Web 登录、各适配器开关等。
   * - ``main/adapters_setup.h``
     - ``minihub_adapters_register_all()`` 声明。
   * - ``main/adapters_setup.generated.c``
     - 自动生成，勿手改。
   * - ``main/idf_component.yml``
     - 自动生成，勿手改。

构建还会在本目录生成 ``sdkconfig`` 与 ``build/``，不要当源文件提交。RainMaker
源码在可选 submodule ``components/esp-rainmaker/``。

改过 ``manifest.yml`` 后直接编译即可（``build_firmware.py`` 会自动合并
manifest）；``store_preinstall`` 列出 ``examples/plugins/``
（以及 ``MINIHUB_PLUGIN_DIRS``）中的模块 id，打进出厂 ``modstore`` 镜像。
公共 Host 默认列表为空，除非你有意工厂预装。

复制本产品做新目录：:doc:`../host_customization`。工厂烧录与分区：
:doc:`../production`。

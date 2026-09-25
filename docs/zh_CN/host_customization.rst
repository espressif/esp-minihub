Host 二次开发
=============

:link_to_translation:`en:[English]`

在 MiniHub Core 之上交付自有 Host 固件（OEM / 二次开发）。本章给出可逐步执行的
产品创建流程。``application/`` 总览见 :doc:`application/index`，通用产品见
:doc:`application/minihub`。

概述
----

产品即目录 ``application/APP/``，用于选择：

* 板级与分区表
* Sdkconfig 特性（以太网、Matter、Store 等）
* 默认 ``adapters.json`` / 规则 / 场景
* 可选工厂预装 Store 模块

做法
----

1. 把 ``application/minihub`` 复制为 ``application/myhub`` （见下方步骤）。
2. 在 ``myhub`` 里改产品文件（板级、分区、出厂 JSON、sdkconfig 特性等），并添加自己的代码。
3. **共享 Core 仍放在仓库** ``components/minihub_*`` ，由所有产品共用；不要拷进
   ``myhub`` 。
4. 复制出来的 ``main.c`` 一般不用动；开哪些协议栈靠 Kconfig 与生成的
   ``adapters_setup.c`` 。

工厂烧录与 OTA 见 :doc:`production` 。

分步示例：创建 ``myhub``
------------------------

1. **以通用 Host 为模板**

   .. code-block:: bash

      cp -a application/minihub application/myhub

2. **编辑** ``application/myhub/manifest.yml``

   拷贝来的文件已有完整 ``sdkconfig_features`` 。不需要的项（ ``rtsp_cam`` 、
   ``usb_msc`` 等）可删掉以节省 flash。最小示例如下：

   .. code-block:: yaml

      extends: _host_base
      board: esp32_s31_function_coreboard_1
      partition_table: partitions.csv
      store_preinstall: []
      sdkconfig_features:
        - common
        - memory
        - partitions_16m
        - eth
        - wifi_sta
        - dynamic_modules

3. **编辑** ``application/myhub/configs/sdkconfig/app.yml``

   ``features:`` 须与 manifest 的 ``sdkconfig_features`` 对齐（合成 sdkconfig
   以 ``app.yml`` 为准）。

4. 出厂配置（可选）

   * ``application/myhub/configs/adapters.json``
   * ``application/myhub/configs/rules.json`` 、 ``scenes.json``

   产品目录没有该文件时，构建回退到仓库 ``configs/*.json`` 。

   类型文案与控件 **不是** 产品 JSON，而在 Host
   ``components/minihub_device_widget/model/*.csv``。新类型改那里，或在已运行
   Hub 上 ``PUT /api/dev/catalog/…``。设备实例仍各登记一条点
   （:doc:`data_model`；类型目录见 :doc:`type_catalog`）。

5. **添加自己的代码**

   在 ``application/myhub/main/`` （或产品下自建 component）增加源文件，并写入
   ``CMakeLists.txt`` 的 ``SRCS`` 。一般不必改启动流程；开协议栈靠 Kconfig。

   网页侧栏版本（前面加 ``v``）来自 Kconfig ``MINIHUB_PRODUCT_VERSION``
   （默认 ``0.1.0``）。Git 描述仍在系统页单独显示为提交号，不作为这个产品版本。

6. **编译烧录**

   .. code-block:: bash

      ./tools/build_firmware.py -app myhub build
      ./tools/build_firmware.py -app myhub -p PORT app-flash monitor

典型目录树
----------

.. code-block:: text

   application/myhub/
     manifest.yml
     partitions.csv
     configs/
       adapters.json
       rules.json
       scenes.json
       sdkconfig/
         app.yml
     main/
       CMakeLists.txt
       main.c
       adapters_setup.h
       idf_component.yml              # 生成
       adapters_setup.generated.c     # 生成
     boards/                          # 可选板级覆盖

分区
----

通用 Host 的 ``partitions.csv`` 只是一份参考。产品有特殊分区需求时须自行规划，
例如 RainMaker / Zigbee 共用的 ``fctry`` NVS 大小、OTA 槽大小、``storage`` /
``modstore`` 等。Zigbee 栈 NVRAM 使用 ``fctry`` NVS（通过
``storage_partition_name``）；通用 Host 不再单独划分 ``zb_storage`` /
``zb_fct``。

Web 控制台
----------

控制台 HTML 打在 ``storage`` 的 ``/data/web/`` 。换皮
``index.html`` / ``login.html`` 或用 curl 推文件见 :doc:`web_console` 。通用
``CMakeLists.txt`` 只拷 ``components/minihub_web/web/`` ，没有
``application/APP/web/`` 覆盖层。

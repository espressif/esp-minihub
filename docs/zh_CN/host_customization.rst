Host 二次开发
=============

:link_to_translation:`en:[English]`

Core 是共用的，一台出货设备和通用 Host 的差别只有几处：跑在哪块板上、编进了哪些
特性、出厂时带什么配置。把这些差别放进你自己的产品里（OEM / 二次开发）。
``application/`` 总览见 :doc:`application/index`，通用产品见
:doc:`application/minihub`。

概述
----

产品即目录 ``application/APP/``。差别全部落在那里，没有一项需要改 Core：

* 板级与分区表
* Sdkconfig 特性（以太网、Matter、商店等）
* 默认 ``adapters.json`` / 规则 / 场景
* 可选工厂预装商店模块

工厂烧录与 OTA 见 :doc:`production`。

分步示例：创建 ``myhub``
------------------------

1. **以通用 Host 为模板**

   .. code-block:: bash

      cp -a application/minihub application/myhub

   Core 仍放在仓库的 ``components/minihub_*`` 里：自带一份 Core 的产品从此收不到
   Core 的修复。复制出来的 ``main.c`` 一般不用动，开哪些协议栈靠 Kconfig 与生成的
   ``adapters_setup.generated.c``。

2. **编辑** ``application/myhub/manifest.yml``

   拷贝来的文件已有完整 ``sdkconfig_features``。不需要的项（ ``rtsp_cam``、
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

   特性清单只认这一份：合成 sdkconfig 与生成的组件表都读 ``app.yml``，
   ``manifest.yml`` 里的 ``sdkconfig_features`` 只在产品没有 ``app.yml`` 时才当备选，
   所以两边不必逐字对齐（出厂的 ``minihub`` 的 manifest 就多列了一个 ``rainmaker``）。
   每个特性都是一个片段，每个片段到底设置了哪些选项，见
   :doc:`kconfig`。

4. 出厂配置（可选）

   * ``application/myhub/configs/adapters.json``
   * ``application/myhub/configs/rules.json``、 ``scenes.json``

   产品目录没有该文件时，构建回退到仓库 ``configs/*.json``。

   类型文案与控件 **不是** 产品 JSON，而在 Host
   ``components/minihub_device_widget/model/*.csv``。新类型改那里，或在已运行
   Hub 上 ``PUT /api/dev/catalog/…``。设备实例仍各登记一条点
   （:doc:`data_model`；类型目录见 :doc:`type_catalog`）。

5. **添加自己的代码**

   在 ``application/myhub/main/`` （或产品下自建 component）增加源文件，并写入
   ``CMakeLists.txt`` 的 ``SRCS``。一般不必改启动流程；开协议栈靠 Kconfig。

   这些源文件要用的接口按「代码跑在哪里」分栏（总入口 :doc:`api/index` ）；编进这份
   固件的产品代码主要是三页：

   * :doc:`api/minihub_core` —— 点位注册表、状态存储、事件总线、命令路由
   * :doc:`api/minihub_adapter` —— 实现 ``minihub_adapter_t``，以及 Host 调它的顺序
   * :doc:`api/net_api` —— Host 上行（以太网 / Wi-Fi STA）初始化、IPv4 查询与 DNS 诊断

   出厂验证与自动化脚本不编进固件，它们打的是 REST： :doc:`api/host_rest` 。

   系统页的「Host 固件版本」与「Web 版本」是两个独立的值；但「产品/侧栏版本」与
   「固件版本」是同一个，统一由 ``MINIHUB_VERSION`` 给出（默认 ``0.1.0``，侧栏
   前面加 ``v``）。``MINIHUB_VERSION`` 用于 OTA 比较、``Firmwares/<chip>/<ver>/``
   文件夹，并在侧栏/系统页显示为 ``v<ver>``；Git 描述仍单独显示为提交号。

   Web 版本单独由 ``MINIHUB_WEB_VERSION`` 管理（``Webs/v<ver>/`` 文件夹与
   ``window.mhWebVersion``）。Web 与固件发布常不同步，可在 ``menuconfig`` 里单独
   设置，默认跟随 ``MINIHUB_VERSION``；``tools/deploy.sh`` 发布时分别读取这两个值。

6. **编译烧录**

   .. code-block:: bash

      ./tools/build_firmware.py -app myhub build
      ./tools/build_firmware.py -app myhub -p PORT app-flash monitor

硬件变体
--------

一个产品可以带多份板子配方：把额外的 YAML 放进
``application/<app>/configs/sdkconfig/variants/``，再用

.. code-block:: bash

   ./tools/apply_sdkconfig_variant.sh lcd          # variants/lcd.yml -> app.yml
   ./tools/apply_sdkconfig_variant.sh --restore    # variants/default.yml -> app.yml

在两者之间切换。变体是 ``app.yml`` 的整份替换—— 换的是特性列表，不是在它上面打补丁。
你手工写进 ``app.yml`` 的东西（实验室 SSID、多加的一个特性）正是会被盖掉的东西。

一个变体其实牵动两样东西，而 ``variants/<name>.yml`` 只管其中一样。组件集、板子与
ESP-IDF 依赖写在 ``manifest.yml`` 的 ``variants:`` 块里，构建时由
``tools/sync_host_manifest.py`` 挑选：它看 ``app.yml`` 的特性里有没有 ``lcd``，有且
``variants.lcd`` 存在才换那一套组件 —— 这个名字是写死的。换个名字的变体只换
sdkconfig 特性，组件集仍是产品自己那份。

.. warning::

   ``apply_sdkconfig_variant.sh`` 直接覆盖
   ``application/<app>/configs/sdkconfig/app.yml`` 且不留备份：一切换变体，手工改的配方
   就没了；``--restore`` 也不会把它找回来—— 它复制的是 ``variants/default.yml`` ，那是
   一份你自己维护的文件，不是 git 历史。请让 ``variants/default.yml`` 与 ``app.yml``
   保持一致（出厂的 ``minihub`` 产品里两者完全相同），并希望切换后还在的东西放进
   ``local.overrides`` ，这个脚本不碰它。

板级胶水
--------

引脚级配置不在这里手写。板子归 ``esp_board_manager`` 管，``app.yml`` 用 ``board:`` 点名它，
构建再从这份定义重新生成 ``application/<app>/components/gen_bmgr_codes/`` ：

.. code-block:: bash

   idf.py --preview bmgr -b esp32_s31_function_coreboard_1
   idf.py --preview bmgr -c ./boards -b <name>   # 自己在 application/<app>/boards/ 里定义的板子

ESP-IDF 仍把 S31 当预览目标，所以每个 ``idf.py`` 调用都要加 ``--preview``（ ``tools/build_firmware.py``
会替你加）。那个生成目录被 gitignore，而且在新签出的仓库里可能还是构建脚本先放下的占位版本——
Component Manager 还没把 ``esp_board_manager`` 拉下来时就是这样。所以第一次配置如果报解析不到
``REQUIRES: esp_board_manager``，解法是 ``idf.py --preview reconfigure``，不是去改那些文件：
下一次构建会把你改的东西覆盖掉。

会一路影响到产品配置的板级选择有两个：以太网与 RGB LCD 共用引脚组（``MINIHUB_LCD_UI`` 声明的是
``depends on !MINIHUB_NET_ETH``），以及 PHY 地址、它的复位 GPIO 和芯片专用初始化。它们都是选项
而不是写死的常量——每个都带缺省值和「谁读它」列在 :doc:`kconfig` 里。每次刷新依赖之后请重新跑
``./tools/apply_component_patches.sh`` ：刚拉下来的 ``managed_components`` 会把 ``patches/`` 里的
内容悄无声息地还原。

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

控制台 HTML 打在 ``storage`` 的 ``/data/web/``。换皮
``index.html`` / ``login.html`` 或用 curl 推文件见 :doc:`web_console`。通用
``CMakeLists.txt`` 只拷 ``components/minihub_web/web/``，没有
``application/APP/web/`` 覆盖层。

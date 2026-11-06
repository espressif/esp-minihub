部署与量产
==========

:link_to_translation:`en:[English]`

工厂镜像、现场升级，以及随之发布的商店包，构成发布路径。实验室首次上电见
:doc:`getting_started`；产品目录与分区规划见 :doc:`host_customization`。

实验室 Wi-Fi 凭据写在 ``local.overrides``：这是被 git 忽略的 sdkconfig 片段，
位置是 ``application/APP/configs/sdkconfig/``。构建把它追加在最后，SSID/PSK 会
进入由该镜像烧录的每一台设备。

厂商云服务的账号密码也不要写进通用的出厂 ``adapters.json``。用户登录后，凭据由
模块保存在自己的存储里（NVS 或模块私有文件）。

工厂烧录
--------

在仓库根目录：

.. code-block:: bash

   # 开发：只烧 app，保留已有 /data 与 /store
   ./tools/build_firmware.py -app minihub -p PORT app-flash monitor

   # 整机：按 flasher_args 重写产品用到的分区
   ./tools/build_firmware.py -app minihub -p PORT flash

   # 仅 商店 分区
   ./tools/build_firmware.py -app minihub -p PORT modstore-flash

   # 仅 Host storage（配置 + /data/web 控制台）
   ./tools/build_firmware.py -app minihub -p PORT storage-flash

   # 先编固件，再合成单文件（成功 build 后也会自动 pack .mhub）
   ./tools/build_firmware.py -app minihub build merge

``build merge`` 在 ``application/minihub/build/minihub_merged.bin`` 生成合并镜像，
供工装烧录。须先成功 ``build``。

分区（通用 minihub，16 MB）
---------------------------

通用 Host 的 ``application/minihub/partitions.csv`` 是起点，不是所有 SKU 的规格。
产品自行规划见 :doc:`host_customization`。

当前表大致包括：

* ``nvs`` — Wi-Fi 凭据、商店 URL、鉴权、OTA 元数据
* ``otadata`` / ``phy_init`` — OTA 状态与射频校准
* ``ota_0`` / ``ota_1`` — 双 app 槽（各 5 MiB）
* ``fctry`` — RainMaker claim / 工厂 NVS；Zigbee 栈 NVRAM 共用此分区（40 KiB）。
  改分区表须整片烧录。
* ``storage`` — LittleFS ``/data`` （ ``adapters.json``、规则、场景；Host Web
  控制台在 ``/data/web/`` ）。替换 Web 见 :doc:`web_console`。
* ``modstore`` — LittleFS ``/store`` （已装 ``.mhub``、工厂 preinstall 覆盖）。

通用 Host **不** 提供 ``zb_storage`` 或 ``zb_fct``。ESP Zigbee SDK 2.x 通过
``storage_partition_name`` 把 NVRAM 存进 NVS（MiniHub 指向 ``fctry``）。
``zb_fct`` （1 KiB FAT）只用于终端设备的可选出厂配置工具；协调器 Host 不用它。
需要 Zigbee 出厂数据的 OEM 须自行增加 FAT 分区，或使用 Zigbee 的 secur /
descriptor API。

Matter 构建需要大 app 槽。链接报装不下时放大 ``ota_0`` / ``ota_1``，不要先缩小
``modstore``。

工厂预装商店
--------------

在 ``application/APP/manifest.yml`` 列出 ``examples/plugins/ID/``
中已有（或 ``MINIHUB_PLUGIN_DIRS`` 拉入）的模块 id。
``module.json`` 里的 ``preinstall`` 字段不生效，见
:ref:`store-preinstall`。打包细节见 :doc:`plugins/pack_and_catalog`。

.. code-block:: yaml

   store_preinstall:
     - color_bulb_js

或工装烧 ``minihub_merged.bin`` / ``storage-flash`` / ``modstore-flash``。

.. only:: not hide_l3

   仓库内 ELF 须打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 并 ``build`` （会生成 ``.so``
   并 pack）。已有制品时：

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub pack
      ./tools/build_firmware.py -app minihub -p PORT flash

   JS 的 id（例如 ``color_bulb_js`` ）同样可以写进 ``store_preinstall``。

规模化配网
----------

1. 烧工厂镜像（NVS 中无 STA 凭据）
2. 首次启动：有网线走以太网；否则在打开
   ``CONFIG_MINIHUB_WIFI_PROVISIONING`` 时走 SoftAP 配网 — 见 :doc:`networking`
3. 操作员打开 ``http://HUB/``，以 ``admin`` 登录（首次访问时设置密码），
   并在需要时完成 Wi-Fi 配置
4. 可选：写入量产商店 URL（ ``POST /api/store/config``，见
   :doc:`api/plugin_rest` ）

   .. code-block:: JSON

      { "custom_url": "https://store.example", "disable_default": true }

恢复出厂
--------

``sdkconfig_features`` 含 ``factory_reset_gpio61`` （或产品等价项）时，长按配置
GPIO 会执行恢复出厂并重启。它删的是用户数据，不会重新格式化文件系统：

* ``/data/config/rules.json``、``scenes.json``、``adapters.json``
  （ :file:`components/minihub_config/minihub_config.c:602` ）
* ``/store`` 下所有由商店安装的模块与包
  （ :file:`components/minihub_store/minihub_store.c:2140` ）
* 整个 NVS，因此 Wi-Fi 凭据、自定义商店 URL 与控制台登录密码一并被擦
  （ :file:`components/minihub_provision/minihub_factory_reset.c:34` ）

出厂预装（ ``/store/preinstall`` ）与 Web 控制台（ ``/data/web`` ）是刻意保留的，
LittleFS 的 ``/data`` 与 ``/store`` 上的其它内容也还在 —— 工装要干净文件系统请整机
``flash`` 。

.. warning::

   恢复出厂之后，在有人重新设定之前控制台 **没有密码** ：登录哈希存在 NVS 里，会一起被
   擦掉；而 ``POST /api/auth/setup`` 按设计就是免鉴权的，好让新设备能被配置。也就是说，
   谁先打开那台已复位设备的控制台，谁就拥有它。请把复位后的设备视为不安全，直到它自己的
   密码设好；更稳妥的做法是在产线上就把密码设掉，而不是以「待设置」状态出货。

公开产品 ``minihub`` 的 sdkconfig 没有打开 secure boot、Flash 加密或 NVS 加密。
OEM 可在产品 sdkconfig 里打开，并按该芯片的 ESP-IDF 安全指南操作。本指南不规定
eFuse、序列号或数字签名的出厂策略。

SBOM 与漏洞扫描
----------------

MiniHub 仓库是 Apache-2.0，且从源码构建，所以供应链与自有代码的检查要放进你
自己的流程。分两层：

SBOM 与依赖 CVE
~~~~~~~~~~~~~~~~

`esp-idf-sbom <https://github.com/espressif/esp-idf-sbom>`_ 把一个构建好的工程
变成 SPDX 或 CycloneDX 的物料清单（SBOM），并对照 NVD 查已知 CVE。安装后先构建
固件，再运行：

.. code-block:: bash

   pip install esp-idf-sbom
   ./tools/build_firmware.py --app minihub build
   esp-idf-sbom create -o minihub.spdx \
       application/minihub/build/project_description.json
   esp-idf-sbom check minihub.spdx

``check`` 会标出 ESP-IDF 及托管组件里的 CVE。MiniHub 自己的组件
（ ``components/minihub_*``、 ``application/minihub/main`` ）会作为包出现在 SBOM
里，但它们不带 SBOM 元数据、也不在 NVD 中，所以 ``check`` 查不到自有代码的 CVE。
要让这些包可被描述，请在项目根目录和每个自有组件里加一份 ``sbom.yml``
（填 ``version`` / ``license`` / ``supplier`` ）。

自有代码扫描
~~~~~~~~~~~~

自有代码不在任何 CVE 库里，要用静态与动态分析来扫：

* **clang-tidy** （免费、Apache-2.0）—— IDF 构建会写出
  ``application/minihub/build/compile_commands.json``，用仓库根的 ``.clang-tidy``
  配置指着它跑。

  推荐用集成好的 ``lint`` 命令：它会自动发现仓库根的 ``.clang-tidy``，且只扫自有
  代码（跳过 IDF 框架、 ``managed_components`` 和生成出来的 ``build/`` 目录）：

  .. code-block:: bash

     ./tools/build_firmware.py lint                 # 所有自有代码
     ./tools/build_firmware.py lint --lint          # 构建成功后自动 lint
     ./tools/build_firmware.py lint -j 8 path/to/file.c   # 指定文件，8 路并行

  **工具链**：优先用 ESP 打过补丁的 clang —— ``idf_tools.py install esp-clang``
  （其 ``clang-tidy`` 会被自动探测到，位于 ``~/.espressif/tools/esp-clang``，
  原生支持 ``esp32s31`` 的 RISC-V 目标与 ESP newlib）。用 stock LLVM 的 clang-tidy
  也行：此时 ``lint`` 会**自动清洗** ``compile_commands.json`` （改写 ``-march`` /
  ``-mcpu`` / ``-mtune``、丢弃 GCC-only flag、展开 ``@file`` 响应文件、补上工具链
  newlib 头目录）让前端能解析源文件——但它仍无法完整解析 ESP newlib 系统头，vendor
  头里会报若干编译器错误。

  也可以直接调 clang-tidy（同样会读到根的 ``.clang-tidy`` ）：

  .. code-block:: bash

     clang-tidy -p application/minihub/build \
         components/minihub_core/src/minihub_core.c

  或用 LLVM 的 ``run-clang-tidy`` 扫 ``components/minihub_*`` 与
  ``application/minihub/main``。仓库根已提供一份启用 ``bugprone-*``、
  ``clang-analyzer-*``、 ``cert-*``、 ``cppcoreguidelines-*`` 的 ``.clang-tidy``。

* **Fuzzing** （免费开源——LibFuzzer、AFL/AFL++）—— 瞄准不可信的解析入口：
  REST/WebSocket 的 JSON、 ``adapters.json`` / 目录解析、以及 Web 上传。

* **CodeQL** —— 仅对开源与科研免费；闭源或商业产品需要 GitHub Advanced Security
  / 商业授权。Apache-2.0 的 MiniHub 仓库本身可免费使用。

现场 OTA
--------

双槽 OTA（ ``ota_0`` / ``ota_1`` ）写入空闲槽后重启。确认动作发生在 ESP-IDF 的
启动流程里、``app_main`` 之前：完全起不来的镜像会退回上一槽，而只要跑到了
``app_main`` 就会被保留，即使之后行为不对也不会自动回退。详见 :doc:`ota`。

商店与 Host
-------------

.. only:: not hide_l3

   ELF ``.so`` 用 Plugin SDK 与导出白名单解析符号，不跟某一版 Host 固件绑定。
   ``module.json`` 中 ``abi_version``、 ``core_api_min`` / ``core_api_max`` 须覆盖
   当前 Host，且芯片架构一致。不必为每次固件发布重编模块。没有单独的固件版本对照表：
   上述三个字段覆盖当前 Host 即可。契约见 :doc:`api/module_abi`。

现场目录是 ``catalog.json`` + ``.mhub``；工厂预装见上文。

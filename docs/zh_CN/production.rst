部署与量产
==========

:link_to_translation:`en:[English]`

OEM 发布路径：工厂镜像、现场 OTA、Store 包。实验室首次上电见
:doc:`getting_started` ；产品目录与分区规划见 :doc:`host_customization` 。

不要把实验室 SSID/PSK（ ``local.overrides`` ，git 忽略）打进量产镜像。不要把
云密码写进出厂 ``adapters.json`` 。

工厂烧录
--------

在仓库根目录：

.. code-block:: bash

   # 开发：只烧 app，保留已有 /data 与 /store
   ./tools/build_firmware.py -app minihub -p PORT app-flash monitor

   # 整机：按 flasher_args 重写产品用到的分区
   ./tools/build_firmware.py -app minihub -p PORT flash

   # 仅 Store 分区
   ./tools/build_firmware.py -app minihub -p PORT modstore-flash

   # 仅 Host storage（配置 + /data/web 控制台）
   ./tools/build_firmware.py -app minihub -p PORT storage-flash

   # 先编固件，再合成单文件（成功 build 后也会自动 pack .mhub）
   ./tools/build_firmware.py -app minihub build merge

``build merge`` 在 ``application/minihub/build/minihub_merged.bin`` 生成合并镜像，
供工装烧录。须先成功 ``build`` 。

分区（通用 minihub，16 MB）
---------------------------

通用 Host 的 ``application/minihub/partitions.csv`` 是起点，不是所有 SKU 的规格。
产品自行规划见 :doc:`host_customization` 。

当前表大致包括：

* ``nvs`` — Wi-Fi 凭据、Store URL、鉴权、OTA 元数据
* ``otadata`` / ``phy_init`` — OTA 状态与射频校准
* ``ota_0`` / ``ota_1`` — 双 app 槽（各 5 MiB）
* ``fctry`` — RainMaker claim / 工厂 NVS；Zigbee 栈 NVRAM 共用此分区（40 KiB）。
  改分区表须整片烧录。
* ``storage`` — LittleFS ``/data`` （ ``adapters.json`` 、规则、场景；Host Web
  控制台在 ``/data/web/`` ）。替换 Web 见 :doc:`web_console` 。
* ``modstore`` — LittleFS ``/store`` （已装 ``.mhub`` 、工厂 preinstall 覆盖）。

Matter 构建需要大 app 槽。链接报装不下时放大 ``ota_0`` / ``ota_1`` ，不要先缩小
``modstore`` 。

工厂预装 Store
--------------

在 ``application/APP/manifest.yml`` 列出 ``examples/plugins/ID/``
中已有（或 ``MINIHUB_PLUGIN_DIRS`` 拉入）的模块 id。
选型与 ``module.json`` 的 ``preinstall`` 无效，见
:ref:`store-preinstall` 。打包细节见 :doc:`plugins/pack_and_catalog` 。

.. code-block:: yaml

   store_preinstall:
     - color_bulb_js

或工装烧 ``minihub_merged.bin`` / ``storage-flash`` / ``modstore-flash`` 。

.. only:: not hide_l3

   仓库内 L3 须打开 ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 并 ``build`` （会生成 ``.so``
   并 pack）。已有制品时：

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub pack
      ./tools/build_firmware.py -app minihub -p PORT flash

   JS 的 id（例如 ``color_bulb_js`` ）同样可以写进 ``store_preinstall`` 。

规模化配网
----------

1. 烧工厂镜像（NVS 中无 STA 凭据）
2. 首次启动：有网线走以太网；否则在打开
   ``CONFIG_MINIHUB_WIFI_PROVISIONING`` 时走 SoftAP/BLE 配网 — 见 :doc:`networking`
3. 操作员打开 ``http://HUB/`` ，完成 Wi-Fi（如需要）与 Store 登录
4. 可选：写入量产 Store URL（ ``POST /api/store/config`` ，见
   :doc:`api/plugin_rest` ）

   .. code-block:: json

      { "custom_url": "https://store.example", "disable_default": true }

恢复出厂
--------

``sdkconfig_features`` 含 ``factory_reset_gpio61`` （或产品等价项）时，按住配置
GPIO 会擦除 NVS 并重启。LittleFS ``/data`` 与 ``/store`` 不会因此清空 —
工装要干净文件系统请整机 ``flash`` 。

公开产品 ``minihub`` 的 sdkconfig 没有打开 secure boot、Flash 加密或 NVS 加密。
OEM 可在产品 sdkconfig 里打开，并按该芯片的 ESP-IDF 安全指南操作。本指南不规定
eFuse、序列号或数字签名的出厂策略。

现场 OTA
--------

双槽 OTA（ ``ota_0`` / ``ota_1`` ）写入空闲槽后重启。新固件若跑到
``minihub_ota_init`` 会取消回滚，否则 bootloader 回到上一槽。详见 :doc:`ota` 。

Store 与 Host
-------------

.. only:: not hide_l3

   L3 ``.so`` 用 Plugin SDK 与导出白名单解析符号，不跟某一版 Host 固件绑定。
   ``module.json`` 中 ``abi_version`` 、 ``core_api_min`` / ``core_api_max`` 须覆盖
   当前 Host，且芯片架构一致。不必为每次固件发布重编模块。没有单独的固件版本对照表：
   上述三个字段覆盖当前 Host 即可。契约见 :doc:`api/module_abi` 。

现场目录是 ``catalog.json`` + ``.mhub`` ；工厂预装见上文。

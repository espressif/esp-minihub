Kconfig 选项
============

:link_to_translation:`en:[English]`

MiniHub 的每一个构建选项都在这里，文档构建时直接从 Kconfig 片段里生成，而不是靠人
维护一份清单。 ``idf.py menuconfig`` 显示的就是同样的文字；本页额外给出每个符号被
哪些文件读取，好让你看得出把它打开到底会不会改变什么。

权威来源是 ``cmake/minihub_bundle/Kconfig.projbuild`` 里的 ``rsource`` 列表：构建挂载
的正是这一份。磁盘上有片段却没被 ``rsource``、``rsource`` 指向的文件已删除、某个符号
没有类型、``int`` 没有缺省值—— 任何一条都会让文档构建失败，而不是发出一份更短的清单。

这里的缺省是 **Kconfig** 缺省，也就是一台还没有 ``sdkconfig`` 的设备会拿到的值。你手上
那块板的实际值取决于它的 ``sdkconfig``，两者通常不一样。

一次构建是怎么得到这些值的
--------------------------

产品的选项不是一条条手写的。 ``application/<app>/configs/sdkconfig/app.yml`` 列的是
*特性*，每个特性是一个成组设置符号的片段。构建先把这份列表解析开，再追加本地覆盖，
把整条链作为 ``SDKCONFIG_DEFAULTS`` 交给 ESP-IDF。靠后的条目盖住靠前的，所以生成的
射频栈片段要排在产品特性之后。

.. minihub-features:: minihub

这条链上有三条规则，改动会不会生效由它们决定：

* 已经存在的 ``sdkconfig`` 盖过所有片段。如果某个符号仍然写在 ``application/<app>/sdkconfig``
  里，那么改 ``feature/*.defaults`` 再重新构建是什么效果都没有的—— 这正是
  ``tools/build_firmware.py`` 选择直接改写产品文件里的射频栈符号
  （ ``CONFIG_MINIHUB_BLE_MESH_SDK`` 等）而不指望片段的原因；也是它刻意不把未选中
  的符号重置的原因：那会抹掉手工编辑或一次 ``menuconfig`` 会话的结果。
* 生成的 ``stacks.gen`` 排在产品自己的特性之后，``local.overrides`` 再排在它之后——
  于是只用于实验室的值可以单独放一个文件，永远不会被提交。
* ``board_manager.defaults`` 是刻意不在列表里的，由 ``esp_board_manager`` 自己注入；
  板子的 flash、PSRAM、PHY 设置就是从那里进来的。
* 共用 IEEE 802.15.4 的两个射频栈不会悄悄并存：同时构建 ``thread`` 与 ``zigbee`` 会
  以退出码 2 停下，并告诉你要二选一。

要知道一次已配置的构建到底用了什么值，读生成出来的文件而不是片段：
``application/<app>/build/config/sdkconfig.h``（C 代码看到的）、同目录的
``sdkconfig.cmake`` 与 ``sdkconfig.json``，或者 ``application/<app>/sdkconfig`` 里那些
盖过缺省的值。 ``tools/build_firmware.py`` 在调用构建之前会把解析好的整条
``SDKCONFIG_DEFAULTS`` 打印出来。

核心与内存
----------

.. minihub-kconfig:: Core

.. minihub-kconfig:: Hub

.. minihub-kconfig:: JS plugins

.. minihub-kconfig:: Modules

射频与适配器
------------

.. minihub-kconfig:: Networking

.. minihub-kconfig:: Provisioning

.. minihub-kconfig:: Matter

.. minihub-kconfig:: Thread

.. minihub-kconfig:: Zigbee

.. minihub-kconfig:: BLE Mesh

.. minihub-kconfig:: Modbus

.. minihub-kconfig:: MQTT adapter

控制台、商店与访问
------------------

.. minihub-kconfig:: Web UI

.. minihub-kconfig:: Auth

.. minihub-kconfig:: Store

.. minihub-kconfig:: MCP

.. minihub-kconfig:: RainMaker

外设
----

.. minihub-kconfig:: LCD UI

.. minihub-kconfig:: RTSP camera

.. minihub-kconfig:: USB MSC

哪些必须由产品自己定
--------------------

多数符号可以留在缺省值。下面这几个是产品决策，而不是调优旋钮：

* ``MINIHUB_VERSION`` 与 ``MINIHUB_WEB_VERSION``——两条发布线，见 :doc:`versions`。
* ``MINIHUB_NET_WIFI_STA`` / ``MINIHUB_NET_ETH``——传输方式。``MINIHUB_LCD_UI``
  ``depends on !MINIHUB_NET_ETH``，因为这块板上 EMAC 与 RGB LCD 共用引脚组。
  ``MINIHUB_NET_WIFI_SOFTAP`` 是给「自己在 ``MINIHUB_NET_WIFI_STA`` 之外驱动 Wi-Fi」的
  产品用的；纯以太网的 Host 把它设成 ``n``。Wi-Fi 不会与 Zigbee 或 Thread 并列，
  所以 802.15.4 产品在这里的传输方式只能是以太网（见 :doc:`radios`）。
* ``MINIHUB_WIFI_SSID`` / ``MINIHUB_WIFI_PASS``——编译进固件。留着字面量 ``YOUR_SSID``
  就是「用 NVS 或配网」；真把网络写在这里，等于把凭据写进每台设备的应用二进制，
  不开 flash 加密就能从 flash 里读出来，见 :doc:`security`。
* ``MINIHUB_WEB_LOGIN_AUTH_TOKEN`` —— 缺省为空，而多数部署就该停在空：一旦在这里填值，
  它就盖过 NVS 里每台设备自己的 ``api_token`` ，并用同一份配方构建的所有设备共用同一个
  字符串。只有「这一整套部署的集成方确实共用一个 token」时才填。
* ``MINIHUB_FACTORY_RESET_BUTTON``——缺省关闭，因此误按 BOOT 不会抹掉现场设备。打开后
  ``FACTORY_RESET_GPIO`` / ``_HOLD_MS`` / ``_ACTIVE_LOW`` 才可见，擦除范围见
  :doc:`production`。
* ``MINIHUB_WEB_LOGIN_AUTH``——缺省开启。关掉之后，局域网 REST 对所有能访问 80 端口的
  人敞开。
* ``MINIHUB_STORE_URL``——设备去哪里找目录与固件的完整 URL（含 scheme 与主机，不带末尾
  路径），例如 ``https://minihub.espressif.com``。缺省指向 Espressif 公共商店；运行时的值由
  控制台或 ``PUT /api/store/config`` 设置，编译期的缺省只兜底。设备会在该 base 后追加
  ``/api/catalog`` 与 ``/files/...``，因此不要填入 ``/api/v1/plugins`` 这类路径。
* 射频栈符号（``MINIHUB_MATTER_ADAPTER``、``MINIHUB_THREAD_ADAPTER``、
  ``MINIHUB_ZIGBEE_SDK``、``MINIHUB_BLE_MESH_SDK``）——每一个都会拉进一个很大的组件，
  并改变 ``menuconfig`` 能选什么，所以它们属于产品的特性列表，而不是每次构建。
  ``thread`` 与 ``zigbee`` 不能同时开。

相关
----

* :doc:`host_customization`——做一个产品： ``application/<name>/``、它的配方、种子数据
* :doc:`limits`——那些配置不了的固定容量
* :doc:`production`——烧录、分区表、恢复出厂的擦除范围
* :doc:`security`——每个缺省防住了什么、又假设了什么
* :doc:`api/errors`——某个设置被拒绝时设备会回什么

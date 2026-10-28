名词表
======

:link_to_translation:`en:[English]`

设备模型
--------

Host
    你烧录的固件镜像，以及跑着它的设备。用来和 PC、云服务区分。

固件 与 镜像
    同一件事的两个词，刻意分开用： **固件** 是设备上跑着的软件， **镜像** 是要被烧写、
    校验、量尺寸的那份产物（OTA 槽里的应用二进制、LittleFS 镜像、合并镜像）。所以槽里
    放的是镜像、设备上跑的是固件：「更新失败」说的是镜像，「起不来」说的是固件。

HUB
    本指南 URL（如 ``http://HUB/`` ）里代表 Hub 局域网地址的占位符。它就是设备打印的
    ``eth got IP`` / ``wifi got IP`` 地址，不是固件注册的主机名。

Core
    ``components/minihub_core`` 及其近邻里的共享代码。Core 知道设备的 *形状*，
    不知道设备的 *种类*。*形状* 即设备的“结构/骨架”，*种类* 即设备的“具体类别”。
    Core 是协议无关、类型无关的通用引擎：它对所有设备一视同仁地按 *点位* 处理；
    新增一种设备品类，只要适配器照常写点位、目录里加一行渲染规则即可，不用改 Core。

产品
    ``application/`` 下的一个目录，目录名就是产品名（通用 Host 是
    ``minihub`` ）。产品带着板级、分区表、sdkconfig 与出厂 JSON。见
    :doc:`application/index`。

SKU（Stock Keeping Unit ，库存单位）
    产品（Product）的一个具体可交付变体：一个 ``application/<sku>/`` 目录，把板级、
    分区表、sdkconfig、出厂 JSON 的差异留在里面，同时共用同一份 Core。一份 Core 服务
    所有 SKU；见 :doc:`application/index`。

.. _point:

点位（point）
    Core 眼里的一台设备：一条 ``minihub_point_t``，有 id、类型名、一个概要
    value、一段参数 JSON、拥有它的适配器、以及在线标志。一台物理设备即是一个点位，不应为同一台设备注册多个点位。见 :doc:`data_model`。

``point_id``
    这条点的地址，写法是 ``<protocol>.<device>``：例如 ``zigbee.zb_7d4b``、
    ``thread.matter_n2_ep1``、 ``sim.js_kitchen``。前缀标的是登记这条点所用的
    传输，不来自设备类型：射频适配器用的是它自己的名字（ ``zigbee.``、
    ``ble_mesh.`` ），Matter 用的是 **配网时的** 传输
    （ ``wifi.`` / ``ethernet.`` / ``thread.`` ）。

类型（type）
    设备的品类，比如 ``light`` 或 ``switch``，它决定采用什么样的图标来渲染设备。

参数（parameter）
    设备的一个实时设置，保存在 ``props.params.<name>``，可带 ``access``、
    ``visible``、 ``min``、 ``max``、 ``step``、 ``unit``。例如 ``power``、
    ``brightness``、 ``cct``。

类型目录（设备控件）
    说明某个类型和它的每个参数如何渲染的表：文案、控件、图标、量程。真值来源是
    ``components/minihub_device_widget/model/*.csv``，设备在启动时读取展平后的
    副本 ``/data/config/devices.csv``。见 :doc:`type_catalog`。

控件（widget）
    目录可以指定的控件种类，封闭的六种：``toggle``、 ``slider``、
    ``dropdown``、 ``text``、 ``button``、 ``hidden``。

适配器（adapter）
    说一种协议、把结果写进点位的代码。通用 Host 自带七个：wifi、zigbee、
    ble_mesh、matter、thread、mqtt、modbus（ :doc:`radios` ）。每个由
    ``adapters.json`` 里 ``adapters`` 下的一个键开关。

关联（associate）
    把一条已注册的点关联到某个插件适配器名下（写入 ``props.owner`` ）。通用设备
    即使不挂用户的适配器也能适配成功；但 OTA 这类需要差异化处理的功能，必须
    关联到对应适配器才能完成。关联永远不会创建第二条点。

规则（rule）
    ``/data/config/rules.json`` 里的一条 ``when`` / ``if`` / ``then`` 自动化，
    在设备上求值。见 :doc:`rules`。

场景（scene）
    作为整体调用的一组命令，走 ``POST /api/scenes/invoke``。场景不是规则的一种
    ``then`` 动作。

事件总线
    设备固件内的发布订阅，每次**发生变化**的点位更新都要经过它。 ``/ws/events``
    上的 WebSocket 是它的镜像；控制台和你的集成订阅的是 ``point.updated``。

打包模型
--------

商店（Store）
    两件事：设备侧子系统 ``minihub_store`` （刷新目录、安装、升级、卸载），
    以及喂给它插件的那台服务器。见 :doc:`plugins/pack_and_catalog`。

QuickJS
    运行 L2 模块的内嵌 JavaScript 引擎。本固件用的是 ``espressif/quickjs-ng``
    组件（QuickJS-NG 0.14.0），不是上游 QuickJS，也不是浏览器：没有模块系统、
    没有 ``console``、没有 ``fetch``。它是单线程的，固件里所有 L2 模块共用一个
    工作任务。具体影响列在 :doc:`plugins/develop_l2`。

.. only:: not hide_l3

   还有第三层 L3，它带的是一段原生 ``.so``，而不是数据或 JavaScript。

   MHMD
       L3 包内层原生镜像的魔数 ``0x4D484D44``。它说的是那个二进制，不是外面的
       zip。

   ABI
       原生模块与 Host 之间的 C 契约：一张 Host 函数表，加上允许符号的哈希。
       ``abi_version`` 是 1， ``MINIHUB_CORE_API_VERSION`` 也是 1。原生模块
       只能链接 ``docs/module_sdk/host_export_allowlist.json`` 里列出的符号；
       契约写在 :doc:`api/module_abi`。

   ``core_api_min`` / ``core_api_max``
       一个包声明可运行的 Host Core API 范围。只有固件版本落在范围内，模块才
       会加载。

运行时与存储
------------

``storage`` 分区
    挂在 ``/data`` 的 LittleFS 卷：Hub 配置、规则、场景、展平后的类型目录，以及
    控制台。

``modstore`` 分区
    挂在 ``/store`` 的 LittleFS 分区： ``/store/modules/`` 下的已安装商店模块，
    某些产品上还有一份预装控制台副本。

NVS
    ESP-IDF 的键值存储。

热重载（hub reload）
    不重启就重启适配器与模块： ``POST /api/hub/reload`` 或 MCP ``hub.reload``。
    它重新读取 JSON、停掉适配器、重载模块，并重新执行每个 L2 的
    ``setup(host)``。Matter 与 Thread 构建可能在重载后把 CHIP 重新挂到
    OpenThread。

一词两义
--------

控制台（Web 控制台）
    设备从 ``/data/web`` 提供的浏览器界面。可以换皮或整个替换
    （ :doc:`web_console` ）。不要和串口终端混淆。

MCP
    两个不相干的东西共用这个名字。 **设备侧** MCP 服务器
    （ ``CONFIG_MINIHUB_MCP_SERVER``，需要 ``components/esp-claw`` ）让 AI 助手
    驱动一台在线 Hub。 **插件作者用的 IDE MCP** （ ``tools/minihub_plugin_mcp.py`` ）
    跑在你的 PC 上，只做脚手架、校验与打包。

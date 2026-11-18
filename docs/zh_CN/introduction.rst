简介
====

:link_to_translation:`en:[English]`

固件做什么
----------

一份 MiniHub 固件就是一个镜像，下面这些事都跑在设备上。

内置适配器覆盖 Wi-Fi STA、Zigbee、BLE Mesh、Matter、作为边界路由的 Thread、
MQTT 与 Modbus。规则存在设备上，
也在设备上求值（ :doc:`rules` ）；Web 控制台、REST API 与 WebSocket 事件流同样是
设备自己提供的，因此外网中断时，局域网内仍可控制。

固件里没有的协议或云能力，事后还能补进来，因为商店模块是运行时安装的，不必
重新编译（ :doc:`plugins/overview` ）。控制台和你自己的集成走的是同一套 REST、
WebSocket 与 JavaScript 接口（ :doc:`api/index` ），可选的 MCP 服务暴露的也是这一
套：打开 ``CONFIG_MINIHUB_MCP_SERVER`` 之后，助手调的就是浏览器在调的那些操作
（ :doc:`api/mcp` ）。

谁在读本指南
------------

.. list-table::
   :header-rows: 1
   :widths: 18 40 42

   * - 读者
     - 你在决定什么
     - 需要读的章节
   * - 评估这套方案
     - 协议覆盖、断网行为、许可证、限制
     - 本页、 :doc:`architecture`、 :doc:`radios`、 :doc:`faq`
   * - 产品工程师
     - 板级、分区、功能开关、出厂配置、现场升级
     - :doc:`getting_started`、 :doc:`application/index`、
       :doc:`host_customization`、 :doc:`production`
   * - 插件作者
     - 接一个开发者云或一类设备，不改 Core
     - :doc:`plugins/first_plugin`、 :doc:`plugins/overview`、
       :doc:`api/js_host`
   * - 接入方
     - 你自己的 App、云或控制台对接 Hub
     - :doc:`api/host_rest`、 :doc:`data_model`、 :doc:`rules`、
       :doc:`data_flow`

这几条路都不要求你先读懂 Core 的 C 源码。契约是 JSON 文件、HTTP 接口，以及
一份模块 ABI。

为什么这样分层
--------------

分层来自一个要求：中枢要兼容它编译那天还不存在的生态，而且这些生态都不能变成
固件的构建依赖。

.. list-table::
   :header-rows: 1
   :widths: 14 30 56

   * - 层
     - 它管什么
     - 这条边界为什么在那儿
   * - Core
     - 点位注册表、命令路由、事件总线、配置文件
     - Core 只保存设备的类型 *名字* 加上一段参数 JSON，所以出现新型设备不需要
       改 Core（ :doc:`data_model` ）
   * - 适配器
     - 协议流量进，点位更新出
     - 规则引用的是点位，所以设备换射频时自动化不用重写（ :doc:`radios` ）
   * - 类型目录
     - 每个类型名的文案、控件、图标、量程
     - 展示是表格数据，新增一个设备类型是一行 CSV，不是一个 C 结构体
       （ :doc:`type_catalog` ）
   * - 商店模块
     - 厂商云、额外设备族、模块自带 UI
     - 可以在现场安装升级；凭证由它们携带，通用构建里因此不需要有任何厂商密钥
       （ :doc:`plugins/index` ）
   * - 产品
     - 板级、分区表、sdkconfig、出厂 JSON
     - SKU 差异留在 ``application/`` 里，多个产品共享同一份 Core
       （ :doc:`application/index` ）

不需要厂商账号或服务端就能工作的协议栈，可以静态链进 Host。凡是要靠账号或厂商
服务端才跑得通的，都以商店模块或你自己的产品树交付。

限制
----

* 编译期没人检查厂商设备类型，因为 Core 只把类型存成一个名字。所以 ``type``
  写错、参数写错都是运行时才暴露：卡片渲染出来却没有控件，控制台记一条告警
  （ :doc:`type_catalog` ）。
* 所有 JS 模块共用一个 QuickJS 任务。一个回调卡住，就会连带拖住其它 JS 模块
  （ :doc:`api/js_host` ）。
* Wi-Fi 与 BLE Mesh 能在 2.4 GHz 射频上共存；IEEE 802.15.4（Zigbee **或**
  Thread）与两者都不共存，Zigbee 与 Thread 也不会一起启动（ :doc:`radios` ）。
* 控制台是局域网明文 HTTP：Host 的 Web 栈没有 TLS 监听。要么用会话登录、要么
  带一个可选的 API token，真需要 TLS 就把它放在 Hub 前面（ :doc:`api/host_rest` ）。
* 通用布局默认要两块可写分区， ``storage`` 放 Hub 配置 **与**  Web 控制台、
  ``modstore`` 放已装模块，所以 Flash 小一点的料可能放不下（ :doc:`production` ）。

下一步
------

编译烧录见 :doc:`getting_started`。分层、启动顺序、存储与热重载见
:doc:`architecture`。一个数值从设备到浏览器再回来见 :doc:`data_flow`。
生词见 :doc:`glossary`，目录见 :doc:`source_tree`。

项目以 Apache License 2.0 发布，协作说明见 :doc:`contributing`。

排障
====

:link_to_translation:`en:[English]`

自上而下排查：先看网络，再看 Hub 本身，最后看具体功能。每一节给一个现象、一个能区分
原因的检查看法、以及修法。日志按固件原样引用，你可以直接在串口输出里搜这些字串。

控制台和 REST API 都没有日志级别开关。详细程度是编译期的 ESP-IDF 设置，另有几个子系统
在启动时自己抬高 tag （ :file:`components/minihub_zigbee/zigbee_stack.c:2296` ）。串口是
唯一的实时日志通道，因此若你需要的那行被编译掉了，请降低阈值重新编译。

控制台页面打不开
----------------

按顺序走这张梯子，每一层都有自己的判据。

.. list-table::
   :header-rows: 1
   :widths: 26 40 34

   * - 检查
     - 判据
     - 不通过时
   * - 链路已起
     - 串口日志出现 ``eth got IP`` 或 ``wifi got IP``
     - 网线没供电 / 无 PoE，或 Wi-Fi 从未关联成功 —— 见下节
   * - 地址就是你输的那个
     - 同一行日志会打印它；Hub **不注册** 主机名
     - 用日志里那个地址；DHCP 租约在重启后可能变
   * - 默认出口选对了
     - ``minihub_net_refresh_default_netif()``：以太网只要有 IPv4 就优先
     - 两条上行都在但你在错的网段 —— 可达的那个是以太网地址
   * - 控制台文件在
     - ``GET /web/index.html`` 返回 HTML；文件在 ``storage`` 分区上
     - 补一次 ``storage-flash``，或在运行中的 Hub 上 ``PUT /api/dev/web/``
   * - 80 端口确实是 Hub
     - ``curl -s http://HUB/api/auth/status`` 返回 JSON
     - 该地址被别的 HTTP 服务占了，或设备正处在配网 SoftAP
   * - 会话还活着
     - ``GET /api/auth/status``；会话失效时页面会跳 ``/login``
     - 空闲 300 秒过期，最长不超过 8 小时 —— 重新登录即可

设备起来了但搜不到那个 Wi-Fi
----------------------------

* 串口日志走的是配网分支（名为 ``PROV_MiniHub`` 的开放热点，信道 1）而不是
  ``wifi got IP`` → 保存过的凭据是空的。连上那个热点，通过它提供的控制台把凭据
  交给 ``POST /api/net/wifi`` 。
* 一直重试但关联不成 → 密码错、隐藏 SSID，或 SoC 用不了的信道。 ``esp_wifi`` 会打原因，
  从日志里读。
* Wi-Fi 配好了但一直拿不到地址 → DHCP 池耗尽，或 AP 开了客户端隔离。用
  ``GET /api/system`` 确认 ``local_ip`` 是不是你预期的那个。
* 本 Host 没有 BLE 配网。Wi-Fi 只能从网页控制台或 ``POST /api/net/wifi`` 进来。

规则不触发
----------

* ``when`` 是边沿触发：点位 **进入** 命中状态的那一刻才触发。值一直保持相同就不会产生
  ``point.updated`` ，也就没有可比的东西 —— 请把它关掉再打开，或者从控制台触发一次。
* 连续重复的相同更新会被防抖窗口合并（ ``CONFIG_MINIHUB_POINT_DEBOUNCE_MS`` ，缺省
  1000 毫秒）。
* ``if`` 条件只对照实时点位判定，从不触发任何东西： ``when`` 命中而 ``if`` 不成立的规则
  就是什么都不做。
* 定时规则与 ``once`` 规则每秒轮询一次，但需要一个像样的时钟 —— 见 :doc:`time` 。从未
  连到 NTP 服务器的 Hub 不会跑任何计划，而且它是静默不跑：点位触发的规则照样有效。
* 当时正在重载：MCP 会答 ``HUB_NOT_READY`` ，而 16 个的动作队列会丢掉放不下的那些。
* 文档根本没加载： ``GET /api/rules`` 返回的是当前生效内容；引擎会记被拒的文档。导入体积
  有上限，超限的文档是整份拒绝，不会被截断后收下。

设备卡片不见了或置灰
--------------------

* 点位在，但被隐藏： ``visible`` 由实例决定（ ``devices.json`` → ``props.params`` ），
  ``access: "R"`` 是只给显示、不给操作。
* 类型或参数不在类型目录里：UI 拿类型目录与活动点位取交集，目录不认识的参数什么都不渲染。
  扩展过模型就把两份 CSV 一起上传 （ :doc:`type_catalog` ）。
* ``enabled: false`` 只在显式写了 false 时才跳过。
* 适配器登记用的 ``point_id`` 与规则或卡片预期的不一致：一台设备一条点，
  ``<protocol>.<device>`` ，参数在 ``props.params`` 里。

商店刷新或安装卡住
------------------

* 已有刷新在跑时， ``POST /api/store/refresh`` 仍然回 **200** 并带 pending 状态：请改为轮询
  ``GET /api/store/refresh`` 的状态，而不是反复提交。
* 安装返回的 400 / 404 / 409 各只对应一件事 （ :doc:`faq` ）。
* 到不了的部署树会显得很慢：探测刻意不占 HTTP 任务，所以控制台照旧能应答 —— 反过来讲，
  在你去读 hint 状态之前，这次失败是看不见的。
* ``GET /api/store/config`` 显示真正生效的 base URL；多写一个结尾斜杠或 ``chip`` 过滤不上，
  目录就会静默变空，不报错。

升级失败，或 Hub 又回到旧版本
-----------------------------

* 镜像先暂存到外部 LittleFS 介质，再由 Vibe Mode 搬运到 ``ota_0`` ：校验和或尺寸不过
  会在搬运时报错，而重启后的恢复路径是 Vibe Mode 重新搬运。若 app 已装不进该槽，到产品
  `partitions.csv <https://gitlab.espressif.cn/ae_group/minihub/esp-minihub/-/blob/master/application/minihub/partitions.csv>`__
  放大 ``ota_0`` 。
* 暂存完成后有几秒连不上：800 毫秒后进 Vibe Mode，搬运完再重启一次，由 bootloader
  确认新固件。
* 升级后仍停在 Vibe Mode → ``ota_0`` 不是有效镜像；保留介质并重启，让 Vibe Mode 重试，
  或读串口日志看 ``minihub/vibe_result`` 记录的结果。
* 固件升级后控制台没变：固件 OTA 从不替换 ``/data/web/`` ，控制台是单独的包
  （ :doc:`web_console` ）。

插件加载不了
------------

开机日志那行就写着原因；每一条都在 :doc:`faq` 里解释过，包括
``JS module 'X' missing main.js`` 、 ``skip module 'X' — adapter 'Y' already registered``
与 ``module table full`` 。

求助时请附上
------------

这五样齐了，通常一轮就能定位：

1. ``GET /api/system`` 的响应（版本、 ``idf_version`` 、 ``commit`` 、芯片、内存）。
2. 从上电到出错的完整串口日志，不只是最后几行。
3. 涉及的文档或包： ``GET /api/rules`` 、 ``GET /api/adapters`` ，或那个商店包的
   ``module.json`` 。
4. 你按什么顺序做了什么，以及确切现象。
5. 它在 ``POST /api/system/reboot`` 之后能否复现 —— 必须重启才重现的现象是启动顺序或时序
   问题，这决定了该往哪儿看。

相关： :doc:`faq`、 :doc:`networking`、 :doc:`rules`、 :doc:`ota`、 :doc:`time`、
:doc:`limits`、 :doc:`api/errors`。

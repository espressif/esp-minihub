安全模型
========

:link_to_translation:`en:[English]`

这一章说清 Host 默认保护什么、对你的网络做了什么假设、以及出货前 OEM 必须自己补上
什么。它描述的是已有行为，不是承诺：公开构建是按「可信局域网」写的。

默认打开的防护
--------------

* **只有一个管理员。** ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` 默认打开，除登录页外，
  所有会改东西的接口与控制台页面都要会话。用户名是固定的（ ``admin`` ，
  ``MINIHUB_WEB_USERNAME`` ），密码在设备上设：6–64 个字符，存在 NVS 命名空间
  ``minihub`` （ :file:`components/minihub_auth/minihub_auth.c:260,302` ）。
* **会话很短。** 空闲 300 秒过期（ ``MINIHUB_AUTH_SESS_SEC`` ），最长不超过 8 小时
  （ ``MINIHUB_AUTH_SESS_ABS_SEC`` ）。没有多用户、没有角色、没有按人的操作历史：
  需要可追溯就把它做在你的云或插件里，而不是 Hub 里。
* **可选的 Bearer token** 是给自动化用的，而它是随固件发出去的默认值：
  ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` 缺省是 ``espressif`` ，只要非空就压过 NVS 里的
  ``api_token`` 。保持默认，等于该产品每一台设备都认同一个 token 。请按产品改掉它，
  或者清空它、只靠 cookie 。

哪些是按设计公开的
------------------

这些不需要会话就能应答，审计设备时应当预期它们是开的：``/``、``/login``、
``/web/*``、``/images/*``、``/modules/*``、``/favicon.ico``、``GET /api/`` ，以及
``/api/auth/status · setup · login · logout`` 。也就是说控制台 HTML、设备图标和各模块
自己的 web 页面，任何能连到设备的人都读得到；鉴权保护的是 **操作** 与配置，不是这层静态
外壳。不要把密钥放进模块的 web 资源或图标文件里。

没有传输加密
------------

Host 只跑明文 HTTP ，也没有 TLS 监听，所以凭据、Bearer token 与配置都是明文穿过局域网。
请把网络本身当成控制面：独立 VLAN 或 SSID、不给访客可达、需要远程访问就靠你在别处终止的
东西（VPN 或反向代理）。一个能被陌生人坐进来的网络，不适合放 Hub 。

Hub 主动拉取的东西也一样。固件与商店包是按你配置的 URL 去取，并且只做 **结构性** 校验 ——
尺寸与镜像校验和 —— 不做来源认证：没有任何东西验证过签名。谁能控制那个 URL ，谁就能决定
写进 flash 的内容。因此要么用不是「顺手能连上」的 HTTPS 地址、在你的 DNS 里钉死部署主机，
要么打开 secure boot 与 flash 加密（见下），让写进去的镜像仍必须被签名才能运行。

密钥放在哪
----------

* NVS 命名空间 ``minihub`` —— 控制台密码与可选的 API token 。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` —— 缺省为空，而部署就该停在空这个状态：
  此时 REST 只接受会话 cookie 或 NVS 里的 ``api_token`` （命名空间 ``minihub`` ），
  也就是每台设备一个密钥。在这里填值是刻意决定，因为编译进去的字符串盖过 NVS ，并让
  用同一份配方构建的所有设备共用它—— 这正是这个缺省留空而不是填个占位值的原因。
  两种情况下登录仍然必需： token 是满足鉴权的另一条路，不是绕过它的路。
* ``CONFIG_MINIHUB_WIFI_SSID`` / ``CONFIG_MINIHUB_WIFI_PASS`` —— 在这里设置的 Wi-Fi
  凭据会被编译进应用二进制，因此不开 flash 加密，就能从任何一台设备的 flash 镜像里读到。
  请保留缺省的 ``YOUR_SSID``，改用 NVS 或 SoftAP 配网流程。
* ``/store/modules/<id>/`` —— 模块自己的账号材料。厂商云 token 应该放这里或模块私有的
  NVS 键， **绝不** 放在 Host Core 的 ``configs/`` ：写进 ``configs/`` 的内容会被编译并
  烧进该构建的每一台设备（ :doc:`host_customization` ）。
* ``/data/config/*.json`` —— 适配器、规则、场景，以明文存在 LittleFS 上。这些文件里的
  内容只要有会话就能读到；把设备拿到手上接个串口也能读到。
  ``GET /api/config/adapters`` 返回整份文档。

物理接触与恢复出厂
------------------

有两个窗口按设计是不设防的：

* **恢复出厂会清掉密码。** 长按配置的 GPIO （或带 ``{"confirm": true}`` 的
  ``POST /api/system/reset`` ）会擦除 NVS ，而 ``POST /api/auth/setup`` 按设计免鉴权，好让
  新设备能被配置 —— 复位后第一个打开控制台的人就是所有者。复位同时会删掉规则 / 场景 /
  适配器与所有商店安装的模块 （ :doc:`production` ）。
* **配网期间是一个开放热点。** 没有保存过 Wi-Fi 凭据时，Host 会起 ``PROV_MiniHub``
  （信道 1 、无密码）并在它上面提供控制台，所以无线电范围内任何人都能设凭据和密码。
  请在无线电归你的地方配网。

串口就是完全控制：日志之外，只要控制台是开的，还有能替换控制台与目录的路由。
``PUT /api/dev/web/``、``PUT/DELETE /api/dev/assets/``、``PUT/DELETE /api/dev/catalog/`` 只要
``CONFIG_MINIHUB_WEB_UI`` 打开就会编进去；``PUT /api/dev/fs/*`` 只要
``CONFIG_MINIHUB_DEV_FS_UPLOAD`` 打开就会编进去（缺省即开）。它们需要会话，但会话能让它们
往设备上写文件 —— 所以泄露的 token 也是一次文件写入。量产构建请关掉这些，也别在现场依赖它们。

公开构建不提供这些
------------------

下面这些要你自己决定，它们都不是事后能打开的开关：

* **secure boot 、 flash 加密、 NVS 加密都是关的** ，公开 ``minihub`` 的 sdkconfig 也没定
  eFuse 、序列号或签名策略（ :doc:`production` ）。打开它们会改变签名密钥的分发流程，并且
  密钥弄错会永久砖化一台设备 —— 请先在自有构建上按该芯片的 ESP-IDF 安全指南完成验证。
* **没有调试锁定策略。** USB 串口与 JTAG 是否继续开放，要你用 eFuse 自己决定，本指南不做
  规定。
* **没有硬件密钥存储、认证或轮换** —— 凭据在设备上以软件可读的形式存在。
* **没有审计日志。** Hub 记录它在做什么，不记录是谁让它做的。
* **没有漏洞披露渠道** ：开源 Host 目前没定义。要出货的 OEM 应在出货前公布一个，并写明
  报告去处。
* **没有射频认证，也没有 RF 设计输入。** 本仓库交付的是固件：不带测试报告、不带天线或
  匹配指导，也没有 CE / FCC / SRRC / TELEC 的模块证书，所以这里任何东西都不能当作「照它
  做出来的设备就合规」的证据。国家与信道计划、发射功率、共存行为都走 ESP-IDF 的 Wi-Fi
  驱动—— MiniHub 从不覆盖它们—— 而由此产生的认证工作归你的硬件团队与目标市场。

相关： :doc:`web_console` （登录与会话）、 :doc:`production` （烧录、 eFuse 领域、 SBOM 与
CVE 扫描）、 :doc:`host_customization` （产品配置）、 :doc:`api/host_rest` （逐个接口的鉴权）、
:doc:`versions` 。

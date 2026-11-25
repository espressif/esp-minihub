替换 Web 控制台
========================

:link_to_translation:`en:[English]`

控制台为 HTML / CSS / JavaScript，不编入 app 二进制。构建时写入 ``storage``
LittleFS，运行时从 ``/data/web/`` 提供。换风格需更新 ``storage``；仅
``app-flash`` 或固件 OTA 不会替换控制台。Web 可单独 OTA：
``POST /api/update/apply`` （ ``"type":"web"`` ，按 ``/api/web`` 索引拉取）；开发写入见
下文 ``PUT /api/dev/web/`` 。见 :doc:`api/host_rest` 。

源码与设备路径
--------------

通用 Host 源码：

.. code-block:: text

   components/minihub_web/web/
     index.html      GET /
     login.html      GET /login
     manifest.json
     …               其它静态文件对应 GET /web/ 加文件名

``CONFIG_MINIHUB_WEB_UI`` 开启时，``application/minihub/main/CMakeLists.txt`` 将该
目录压缩进 ``storage``，对应 ``/data/web/``。

自定义界面：将 ``MINIHUB_CUSTOM_WEB_SRC``（``menuconfig`` → MiniHub → Web UI）设为
仓库相对路径或绝对路径，留空则用默认。路径须含 ``index.html``，否则构建失败。
:file:`tools/minify_web_assets.py` 用 ``CONFIG_MINIHUB_WEB_VERSION`` 打版本号。

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - 浏览器
     - 设备上的文件
   * - ``GET /``
     - ``/data/web/index.html``
   * - ``GET /login``
     - ``/data/web/login.html``
   * - ``GET /web/`` 加文件名
     - ``/data/web/`` 加同一文件名

``/data/web/`` 缺失时回退 ``/store/preinstall/web/``。``GET /`` 再回退 ROM 提示页
（语言取自 ``Accept-Language``）；该页不可 OEM 换风格，须恢复 ``storage`` 上的
``index.html``。缺少 ``login.html`` 时 ``GET /login`` 返回 404（UTF-8 纯文本）。

``GET /`` 检查会话 Cookie；``GET /login`` 公开。``/web/`` 与 ``/img/`` 无需会话；
鉴权在 ``/api/``。

Kconfig 与登录
--------------

* ``CONFIG_MINIHUB_WEB_UI``（默认开）— 提供控制台。关闭后 ``GET /`` 为 404；
  REST ``/api/`` 仍可用。
* ``CONFIG_MINIHUB_WEB_ASSET_GZIP`` / ``_PLAIN`` / ``_BOTH``（默认 gzip）—
  ``/data/web/`` 落盘方式：

  * **gzip** — 可压缩文件为 ``.gz`` ，省 Flash；请求须带 ``Accept-Encoding: gzip`` ，否则 406。
  * **plain** — 仅明文，兼容不带 gzip 的客户端。
  * **both** — 明文与 ``.gz`` 并存。

  ``login.html`` 、 ``version.txt`` 、 ``manifest.json`` 、 ``release_notes.txt`` 始终明文。
  商店为 ``Webs/<ver>/plain/`` 与 ``…/gzip/`` ；CI 打出
  ``minihub-web-*-plain.zip`` / ``*-gzip.zip`` （无 ``.stamp`` ）。OTA 去掉前缀后写入
  ``/data/web/`` ，只同步与本宏匹配的树。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认开，依赖 Web UI）— 首次 ``/login`` 为
  ``admin`` 设密码（NVS ``minihub/web_pw`` ）。Cookie ``mh_sess`` ：空闲 **5 分钟**
  （仅 ``GET /`` 等文档导航续期，后台 ``fetch`` 不续期）；自登录起最长 **8 小时** 。
  过期后 API 返回 401，控制台跳转 ``/login`` 。关闭后无 Cookie、无 Bearer。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — 可选 ``Authorization: Bearer`` ，默认
  ``espressif`` （开发缺省，按产品修改）。启动时解析一次：Kconfig 非空则忽略 NVS
  ``minihub/api_token`` ；仅 Cookie 时将该项置空。

替换 ``login.html`` 时须调用：

* ``GET /api/auth/status`` — ``login_auth``、``password_set``、``logged_in``
* ``POST /api/auth/setup`` — 首次设密（JSON ``username`` / ``password``；用户名
  ``admin``，密码 6–64 位）
* ``POST /api/auth/login`` / ``POST /api/auth/logout``

``index.html`` 使用同一套 ``/api/`` （点位、命令、设备控件、商店等）。只换 HTML
不会增加固件 API。

概览页 **按已登记点一张卡** 。加载 ``GET /api/device_widgets`` 与
``GET /api/points`` ，用 ``device_widgets.css`` 。类型不在目录时，详情仍画参数行，
浏览器控制台记录缺失（:doc:`type_catalog`）。写入
``POST /api/devices/control`` （ ``device`` 为类型，并带 ``point_id`` ）。类型目录
CSV 可用 ``PUT /api/dev/catalog/…`` 或 ``python3 tools/dev_sync.py --open`` 更新，
无需重编固件。

登录开启时 ``PUT /api/dev/web/`` 须鉴权：先打开 ``/login`` 或带 Bearer，再 curl
附 ``Cookie`` / ``Authorization``。JSON 见 :doc:`api/host_rest`。

构建时替换（工厂 / 实验室）
--------------------------------

1. 修改 ``components/minihub_web/web/`` （或构建前覆盖该目录）。
2. 重新编译以生成 ``storage`` 。
3. 烧录 **storage 分区** ：

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub build
      ./tools/build_firmware.py -app minihub -p PORT storage-flash

``app-flash`` 保留现有 ``/data``。现场固件 OTA 只写 ``ota_0``，不更新
``/data/web/``。合并镜像见 :doc:`production`；OTA 见 :doc:`ota`。

通用 Host 仅从 ``components/minihub_web/web/`` 拷贝；无 ``application/APP/web/``
覆盖层，除非修改该产品 ``CMakeLists.txt``。

运行中替换（不必重烧）
------------------------

界面路径：配置插件服务器后，``系统`` > ``检查更新`` > ``更新网页``
（``POST /api/update/apply``，``"type":"web"``）。

开发路径：``PUT /api/dev/web/<相对路径>``，请求体写入 ``/data/web/``（最大
512 KiB；路径不得含 ``..``）。

.. code-block:: bash

   curl -X PUT http://HUB/api/dev/web/index.html \
     --data-binary @components/minihub_web/web/index.html

   curl -X PUT http://HUB/api/dev/web/login.html \
     --data-binary @components/minihub_web/web/login.html

401 时带 ``Authorization: Bearer`` 或 ``mh_sess`` Cookie（:doc:`api/host_rest`）。

成功响应含 ``path`` 与 ``bytes``。嵌套路径（如 ``css/app.css``）会创建上级目录。
刷新 ``http://HUB/`` 或 ``http://HUB/login`` 时关闭缓存。

本路由不是 ``PUT /api/dev/fs/``（LCD ``/data/ui/``，由
``CONFIG_MINIHUB_DEV_FS_UPLOAD`` 控制）。

.. warning::

   每次 PUT 只替换一个文件。部分上传会导致 HTML 与 JS 版本不一致。须上传全部改动
   文件，或 ``storage-flash`` 恢复镜像控制台。``app-flash`` 不改 ``/data/web``；
   恢复出厂会擦除整个 storage 分区。

语言目录
--------

一份 ``index.html`` / ``login.html``，外加 ``web/i18n/zh_CN.json``、
``web/i18n/en.json``。语言在浏览器侧选择：

``localStorage['minihub.lang']`` → 浏览器语言（``zh*`` → ``zh_CN``，否则 ``en``）→ ``en``。

手动选择优先于浏览器默认；按 origin 隔离。切换后写 key 并重载；目录请求带
``?lang=``。文案加载完成前页面隐藏。

``Accept-Language`` 不映射到分语言页面树；用于 ROM 兜底页与 API 错误文案
（含缺少 ``login.html`` 时的 404）。

商店模块自带的 ``web/`` 不在本机制内。

模块 UI
--------

商店模块可自带 ``web/``（``/store/modules/ID/web/`` → ``/modules/ID/web/…``），
与 Host 控制台无关。见 :doc:`plugins/develop_js`。

替换 Web 控制台
========================

:link_to_translation:`en:[English]`

Host 控制台（HTML/CSS/JS）可由 OEM 换皮。它 **不打进** app 二进制。构建时拷入
``storage`` LittleFS 镜像；运行时 HTTP 从 ``/data/web/`` 读取。

源码与设备路径
--------------

通用 Host 源码：

.. code-block:: text

   components/minihub_web/web/
     index.html      GET /
     login.html      GET /login
     manifest.json
     …               其它静态文件对应 GET /web/ 加文件名

打开 ``CONFIG_MINIHUB_WEB_UI`` 时，
``application/minihub/main/CMakeLists.txt`` 把该目录压缩进 ``storage`` 镜像，对应
``/data/web/`` 。

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

``/data/web/`` 缺少 ``index.html`` 时，固件会再试遗留路径
``/store/preinstall/web/`` （迁移前已烧过的设备），再回退到 ROM 提示页（按
``Accept-Language`` 中/英）。该 ROM 页 **不能** OEM 换皮；应把 ``index.html``
写回 ``storage`` 。缺少 ``login.html`` 时，``GET /login`` 返回 404（UTF-8 纯文本）。

打开 Web 登录后，``GET /`` 与 ``GET /web/…`` 可能需要会话 Cookie；``GET /login``
保持公开，以便显示登录表单。

Kconfig 与登录
--------------

* ``CONFIG_MINIHUB_WEB_UI`` （默认开）— 提供控制台。关掉后 ``GET /`` 为 404，
  REST ``/api/`` 仍可用。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认开，依赖 Web UI）— 首次打开
  ``/login`` 为 ``admin`` 设密码（NVS ``minihub/web_pw``）。Cookie ``mh_sess``：
  **空闲 5 分钟** 无任何鉴权请求则过期（GET 轮询也算在用，会续空闲期）；另有
  **登录后最长 8 小时** 绝对上限。过期后 API 返回 401，控制台跳转 ``/login``，
  避免缓存页继续删插件 / 控设备。关掉后无 Cookie、也无 Bearer。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — 可选 ``Authorization: Bearer`` 给脚本。
  空字符串表示只用 Cookie。NVS 回退键仍是 ``api_token`` 。

替换 ``login.html`` 时须继续调用：

* ``GET /api/auth/status`` — ``login_auth`` 、 ``password_set`` 、 ``logged_in``
* ``POST /api/auth/setup`` — 首次设密（JSON ``username`` / ``password`` ；用户名须为
  ``admin`` ，密码 6–64 位）
* ``POST /api/auth/login`` / ``POST /api/auth/logout``

``index.html`` 须继续使用同一套 ``/api/`` （点位、命令、设备控件、Store 等）。只换 HTML
不会增加固件 API。

设备页 **按已登记点一张卡** （不是每个参数一张卡）。加载
``GET /api/device_widgets`` 与 ``GET /api/points``，用 CSS 控件
（``device_widgets.css``）。类型不在目录里时显示 **类型未登记** 徽标。写入走
``POST /api/devices/control`` （``device`` 是类型；UI 同时带上卡片的
``point_id``，同类型多台可各自控制）。
类型目录 CSV 可不重编固件：``PUT /api/dev/catalog/…`` 或
``python3 tools/dev_sync.py --open``。

打开登录后，``PUT /api/dev/web/`` 也要鉴权。常见顺序：先在浏览器打开一次
``/login`` （或带 Bearer），再用 curl 带 ``Cookie`` 或 ``Authorization`` 。JSON
见 :doc:`api/host_rest` 。

构建时替换（工厂 / 实验室）
--------------------------------

1. 修改 ``components/minihub_web/web/`` 下的文件（或在构建前把皮肤覆盖进去）。
2. 重新编译，让 CMake 重新生成 ``storage`` 。
3. 烧录 **storage 分区**，不要只烧 app：

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub build
      ./tools/build_firmware.py -app minihub -p PORT storage-flash

``app-flash`` 会保留现有 ``/data`` 。**现场 OTA** 只写 ``ota_0`` / ``ota_1`` ，
**不会** 更新 ``/data/web/`` 。实验室 ``app-flash`` 或现场 OTA 之后，
设备仍提供旧 HTML，直到 ``storage-flash`` 或整机 ``flash`` 。合并镜像见
:doc:`production` 。OTA 见 :doc:`ota` 。

通用 Host 只从 ``components/minihub_web/web/`` 拷贝。没有
``application/APP/web/`` 覆盖层，除非改该产品的 ``CMakeLists.txt`` 。

运行中替换（不必重烧）
------------------------

``PUT /api/dev/web/`` 加上相对路径，把请求体写入 ``/data/web/``
下对应文件（最大 512 KiB）。相对路径不得含 ``..`` 。

.. code-block:: bash

   # 控制台（浏览器强制刷新）
   curl -X PUT http://HUB/api/dev/web/index.html \
     --data-binary @components/minihub_web/web/index.html

   # 登录页
   curl -X PUT http://HUB/api/dev/web/login.html \
     --data-binary @components/minihub_web/web/login.html

若 REST 返回 **401** ，与其它 Host API 一样带 ``Authorization: Bearer`` 或
``mh_sess`` Cookie（ :doc:`api/host_rest` ）。

成功响应含 ``path`` （设备 VFS 路径）与 ``bytes`` 。嵌套名如 ``css/app.css`` 会创建
上级目录。再打开 ``http://HUB/`` 或 ``http://HUB/login`` （关闭缓存）。

这不是 ``PUT /api/dev/fs/`` （LCD 的 ``/data/ui/`` ，由
``CONFIG_MINIHUB_DEV_FS_UPLOAD`` 控制）。

语言目录
--------

控制台是 **一份** ``index.html`` / ``login.html``，外加每种语言一份文案文件
（``web/i18n/zh_CN.json``、``web/i18n/en.json``）。语言在浏览器里决定，不由服务端决定：

``localStorage['minihub.lang']`` → 浏览器语言（``zh*`` → ``zh_CN``，否则 ``en``）→ ``en``。

用户手动选过的语言因此优先于浏览器默认值。``localStorage`` 按 origin 隔离，每台
Hub 各自记住自己的选择。切换时写入该 key 并重新加载页面；设备目录会带
``?lang=`` 重新拉取，设备与参数显示名跟随同一个选择。

页面在文案文件加载完成前是隐藏的，所以英文用户不会看到中文首帧。
``/web/i18n/`` 与 ``/web/js/i18n.js`` 是 **不需要** 会话就能取到的两个路径，
因为登录页是用户看到的第一个页面，必须能自己翻译；两者只含界面文案。

这就是 HTTP 服务 **不再** 把 ``Accept-Language`` 映射到 ``web/en/`` 与
``web/zh_CN/`` 目录的原因 —— 根本没有按语言分开的页面树可选。只有 ROM 兜底页
（Web 资源还没挂载时）仍按 ``Accept-Language`` 选中英。

Store 模块自带的 ``web/`` UI 目前还不在这套机制覆盖范围内。

模块 UI
--------

Store 模块可自带 ``web/`` ，位于 ``/store/modules/ID/web/`` ，对应
``/modules/ID/web/…`` ，与 Host 控制台无关。插件 UI 见
:doc:`plugins/develop_l2` 。

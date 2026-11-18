替换 Web 控制台
========================

:link_to_translation:`en:[English]`

控制台就是普通的 HTML、CSS 和 JavaScript，所以 OEM 换皮不必碰固件源码。它
**不打进** app 二进制：构建时拷入 ``storage`` LittleFS 镜像，运行时 HTTP 从
``/data/web/`` 读取。换皮重铺的是 ``storage``，不是 app：只烧 ``app-flash``
或走现场 OTA，设备上的旧控制台不会变。能写进正在运行的 Hub 的有两条路：下文
那条开发用上传路由，以及部署根的更新通道
（``POST /api/update/apply`` 配 ``"type":"web"``，从已发布的 ``/api/web``
索引拉取）—— 见 :doc:`api/host_rest`。

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
``/data/web/``。

要用自定义界面替代内置控制台，把 ``MINIHUB_CUSTOM_WEB_SRC`` （``menuconfig`` →
MiniHub → Web UI）设成仓库相对路径或绝对路径即可，留空则用默认。该路径必须含
``index.html`` ，否则构建会失败。同样的 :file:`tools/minify_web_assets.py` 步骤会用
``CONFIG_MINIHUB_WEB_VERSION`` 给打上版本号，所以自定义皮肤也沿用 Web 版本号的唯一
来源。

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

任何 ``/web/`` 文件在 ``/data/web/`` 下找不到时，固件会再试遗留根
``/store/preinstall/web/`` （迁移前已烧过的设备）才放弃。``GET /`` 还会再退一层
到 ROM 提示页 （按 ``Accept-Language`` 中/英）。该 ROM 页 **不能** OEM 换皮；
应把 ``index.html`` 写回 ``storage``。缺少 ``login.html`` 时，``GET /login``
返回 404 （UTF-8 纯文本）。

只有 ``GET /`` 会检查会话 Cookie；``GET /login`` 保持公开，以便显示登录表单。
整棵 ``/web/`` 静态资源 （HTML、JS、CSS、i18n JSON）和 ``/img/`` 图标都是
**无需会话** 就能取的 —— 它们不含凭证与设备数据，而且若加以限制，会话在页面
加载途中过期时，就会把登录页塞进 ``<script>`` / ``<link>`` 里，造成语法错误与
半截响应。真正把关的是 ``/api/`` 那批接口。

Kconfig 与登录
--------------

* ``CONFIG_MINIHUB_WEB_UI`` （默认开）— 提供控制台。关掉后 ``GET /`` 为 404，
  REST ``/api/`` 仍可用。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH`` （默认开，依赖 Web UI）— 首次打开
  ``/login`` 为 ``admin`` 设密码（NVS ``minihub/web_pw``）。Cookie ``mh_sess``：
  **空闲 5 分钟** 即过期，而只有文档级导航 （打开 ``GET /``、刷新页面）才会
  重置这段空闲：``minihub_auth_check`` 是只读的，控制台后台的 ``fetch`` 轮询
  **不算** 活动，所以自动刷新的页签照样会掉线；另有 **登录后最长 8 小时**
  绝对上限，两者都不延长。过期后 API 返回 401，控制台跳转 ``/login``，
  避免缓存页继续删插件 / 控设备。关掉后无 Cookie、也无 Bearer。
* ``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — 可选 ``Authorization: Bearer`` 给脚本。
  给脚本用。``CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN`` 对所有产品都默认
  ``espressif``，所以新构建直接就认这个 token —— 它是共用的开发缺省值，不是每台
  设备各自的秘密，请按产品换成自己的值。token 只在启动时解析一次，且 **以编进来的
  串为准**：它非空时，NVS 键 ``minihub/api_token`` 根本不会被读，所以先把 Kconfig
  值清空才轮得到 NVS —— 想只走 Cookie，也是这样清空的。

替换 ``login.html`` 时须继续调用：

* ``GET /api/auth/status`` — ``login_auth``、 ``password_set``、 ``logged_in``
* ``POST /api/auth/setup`` — 首次设密（JSON ``username`` / ``password``；用户名须为
  ``admin``，密码 6–64 位）
* ``POST /api/auth/login`` / ``POST /api/auth/logout``

``index.html`` 须继续使用同一套 ``/api/`` （点位、命令、设备控件、商店等）。只换 HTML
不会增加固件 API。

概览页 **按已登记点一张卡** （不是每个参数一张卡）。加载
``GET /api/device_widgets`` 与 ``GET /api/points``，用 CSS 控件
（``device_widgets.css``）。类型不在目录里时，设备详情仍会画它的参数行，
浏览器控制台会记下这次缺失（ :doc:`type_catalog`）。写入走
``POST /api/devices/control`` （``device`` 是类型；UI 同时带上卡片的
``point_id``，同类型多台可各自控制）。
类型目录 CSV 可不重编固件，直接走 REST（``PUT /api/dev/catalog/…``）或
``python3 tools/dev_sync.py --open``。

打开登录后，``PUT /api/dev/web/`` 也要鉴权。常见顺序：先在浏览器打开一次
``/login`` （或带 Bearer），再用 curl 带 ``Cookie`` 或 ``Authorization``。JSON
见 :doc:`api/host_rest`。

构建时替换（工厂 / 实验室）
--------------------------------

1. 修改 ``components/minihub_web/web/`` 下的文件（或在构建前把皮肤覆盖进去）。
2. 重新编译，让 CMake 重新生成 ``storage``。
3. 烧录 **storage 分区**，不要只烧 app：

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub build
      ./tools/build_firmware.py -app minihub -p PORT storage-flash

``app-flash`` 会保留现有 ``/data``。**现场 OTA** 只写 ``ota_0`` ，
**不会** 更新 ``/data/web/``。实验室 ``app-flash`` 或现场 OTA 之后，
设备仍提供旧 HTML，直到 ``storage-flash`` 或整机 ``flash``。合并镜像见
:doc:`production`。OTA 见 :doc:`ota`。

通用 Host 只从 ``components/minihub_web/web/`` 拷贝。没有
``application/APP/web/`` 覆盖层，除非改该产品的 ``CMakeLists.txt``。

运行中替换（不必重烧）
------------------------

Web 控制台也可在界面里更新：配置好自定义插件服务器后，进入 ``系统`` > ``检查更新``
并点 ``更新网页``，Hub 会拉取新包（``POST /api/update/apply`` 配 ``"type":"web"``）。
下面的 ``PUT /api/dev/web/`` 是开发 / 实验室手动写入的替代方式：

``PUT /api/dev/web/`` 加上相对路径，把请求体写入 ``/data/web/``
下对应文件（最大 512 KiB）。相对路径不得含 ``..``。

.. code-block:: bash

   # 控制台（浏览器强制刷新）
   curl -X PUT http://HUB/api/dev/web/index.html \
     --data-binary @components/minihub_web/web/index.html

   # 登录页
   curl -X PUT http://HUB/api/dev/web/login.html \
     --data-binary @components/minihub_web/web/login.html

若 REST 返回 **401**，与其它 Host API 一样带 ``Authorization: Bearer`` 或
``mh_sess`` Cookie（ :doc:`api/host_rest` ）。

成功响应含 ``path`` （设备 VFS 路径）与 ``bytes``。嵌套名如 ``css/app.css`` 会创建
上级目录。再打开 ``http://HUB/`` 或 ``http://HUB/login`` （关闭缓存）。

这不是 ``PUT /api/dev/fs/`` （LCD 的 ``/data/ui/``，由
``CONFIG_MINIHUB_DEV_FS_UPLOAD`` 控制）。

.. warning::

   每次 PUT 只替换 **一个文件** 。这个路由不知道 ``index.html`` 和 ``JS/app.js`` 属于
   同一次构建，所以推一半会留下一台由两代产物拼起来的控制台—— 通常表现为页面能打开、
   随后在浏览器控制台里报错，因为 HTML 和 JS 已经对不上了。要么把你改过的文件全推一遍，
   要么重烧 storage 分区（ ``storage-flash``）回到镜像自带的那版控制台： ``app-flash`` 只写
   应用分区， ``/data/web`` 原样留着；恢复出厂则把整个 storage 分区擦掉，这些文件也一起没。

语言目录
--------

控制台是 **一份** ``index.html`` / ``login.html``，外加每种语言一份文案文件
（``web/i18n/zh_CN.json``、``web/i18n/en.json``）。语言在浏览器里决定，不由服务端决定：

``localStorage['minihub.lang']`` → 浏览器语言（``zh*`` → ``zh_CN``，否则 ``en``）→ ``en``。

用户手动选过的语言因此优先于浏览器默认值。``localStorage`` 按 origin 隔离，每台
Hub 各自记住自己的选择。切换时写入该 key 并重新加载页面；设备目录会带
``?lang=`` 重新拉取，设备与参数显示名跟随同一个选择。

页面在文案文件加载完成前是隐藏的，所以英文用户不会看到中文首帧。这在未登录时
也成立，因为整棵 ``/web/`` （含 ``/web/i18n/`` 与 ``/web/js/i18n.js``）本来就
**不需要** 会话；这些文件只含界面文案。

HTTP 服务 **不会** 把 ``Accept-Language`` 映射到 ``web/en/`` 与 ``web/zh_CN/``
目录 —— 根本没有按语言分开的页面树可选。但 ``Accept-Language`` 仍然决定两处
非页面内容的语言：ROM 兜底页，以及 API 写进错误响应体里的可读文案 （含缺少
``login.html`` 时那个 404）。

商店模块自带的 ``web/`` UI 目前还不在这套机制覆盖范围内。

模块 UI
--------

商店模块可自带 ``web/``，位于 ``/store/modules/ID/web/``，对应
``/modules/ID/web/…``，与 Host 控制台无关。插件 UI 见
:doc:`plugins/develop_js`。

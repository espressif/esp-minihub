第一个商店插件
===================

:link_to_translation:`en:[English]`

从空目录到插件在 Hub 上跑起来，再到不改固件地改它，走一遍。示例用 L2
（QuickJS 上跑 JavaScript），因为它不需要交叉编译。换层时改的只有第 1 步的
``--tier`` 和第 2 步写的内容。

你到底要产出什么
----------------

一个插件就是一个目录。下面这些文件里只有三个归你写，其余由别人生成或别人读：

.. list-table::
   :header-rows: 1
   :widths: 24 30 46

   * - 文件
     - 谁来写
     - 谁来读
   * - ``module.json``
     - 你（身份、 ``runtime``）
     - 加载器：判断这个包能不能在当前 Host 上加载
   * - ``main.js``
     - 你（全部行为）
     - 嵌入式 QuickJS 引擎：在 ``setup(host)`` 里执行
   * - ``meta.json``
     - 你（ ``ui`` 文案、 ``capabilities``）
     - 控制台：据此画该模块自己的面板
   * - ``catalog.json``
     - ``tools/sync_store_catalog.py``
     - ``GET /api/catalog`` 与商店页面 —— 不要手写
   * - ``<id>.mhub``
     - ``tools/pack_store_module.py``
     - Hub：解开到 ``/store/modules/<id>/``

开始前
------

要装到的 Host 必须在编译时开了动态模块与 JS 运行时：
``application/minihub/configs/sdkconfig/app.yml`` 里的 ``dynamic_modules``
（ ``CONFIG_MINIHUB_DYNAMIC_MODULES`` 与 ``CONFIG_MINIHUB_JS_PLUGINS``）。在自己的
构建树里确认 —— 两行都必须是 ``=y``：

.. code-block:: bash

   grep -E "CONFIG_MINIHUB_(DYNAMIC_MODULES|JS_PLUGINS)" \
     application/minihub/build/config/sdkconfig || \
   grep -E "CONFIG_MINIHUB_(DYNAMIC_MODULES|JS_PLUGINS)" application/minihub/sdkconfig

还要一个可写的部署根，默认 ``~/workspace/minihub_store`` （用
``MINIHUB_PLUGINS_DEPLOY_DIR`` 覆盖）。

1. 生成骨架
-----------

.. code-block:: bash

   python3 tools/new_store_module.py --tier l2 --id my_js
   # -> examples/plugins/my_js/{module.json,main.js,meta.json}

``--id`` 既是目录名也是模块 id：小写、下划线。这一步不编译任何东西，也什么都没装。

2. 写行为
---------

``main.js`` 只需要一个函数，Host 在加载和每次 reload 时调用它：

.. code-block:: javascript

   function setup(host) {
       host.point.register({
           point_id: 'my_js.lamp',        // <前缀>.<设备>，模块内唯一
           type: 'light',                 // 必须是类型目录里的 device_id
           adapter: 'my_js',              // 命令按它路由到这里
           value: 'off',
           params: { power: { value: false, access: 'RW' } },
       });
       host.command.on('my_js', function (cmd) {
           if (cmd.cmd === 'turn_on') {
               host.point.set('my_js.lamp', 'on', { params: { power: { value: true } } });
           } else if (cmd.cmd === 'turn_off') {
               host.point.set('my_js.lamp', 'off', { params: { power: { value: false } } });
           }
       });
       host.log.info('my_js ready');
   }

三条决定“能不能用”的硬约束，不是风格问题：

* ``point_id`` 是你这个模块自己取的名字 —— ``<前缀>.<设备>`` 这种形状只是让它在各
  模块之间不撞名，没有人从它推导适配器；仓库里两个演示用的前缀就是 ``sim.``
  （ ``examples/plugins/color_bulb_js`` ）。 ``type`` 必须是类型目录里的
  ``device_id``，否则概览页没有控件（:doc:`../type_catalog`）。
* 命令按 ``point.register()`` 里传的 ``adapter`` 字段路由，你用
  ``host.command.on(adapter, fn)`` 绑它。省略这个字段，点就落在加载器默认的
  ``js`` 适配器上，那时 ``host.command.on('my_js', …)`` 永远不会被调到。
* 一台物理设备一条点，参数放在 ``props.params`` 下 —— 绝不做成一条参数一条点
  （:doc:`../data_model`）。
* 引擎单线程且所有 JS 模块共用：回调里不许死循环。耗时的活交给
  ``host.adapter.poll`` 或 ``setTimeout`` 后立刻返回（:doc:`../api/js_host`）。

3. 打包
-------

.. code-block:: bash

   python3 tools/pack_store_module.py pack my_js
   python3 tools/sync_store_catalog.py my_js
   python3 tools/store_server.py --port 8080

第一条写到 ``~/workspace/minihub_store/Stores/Common/my_js/my_js.mhub``，并在它旁边
把 ``catalog.json`` 一起写好（ ``pack_store_module.py:323-326`` ）；L1、
L2 一律在 ``Common`` 下，原生模块不是商店包，所以 ``pack`` 会跳过它。第二条是你手工放了包、
或改过条目之后才需要跑的 —— 在 ``pack`` 之后它是多余的，留着也无害。那份
``catalog.json`` 是商店侧唯一的索引， ``/api/catalog`` 是请求时聚合
这些逐插件文件。第三条把两者一起提供服务。动 Hub 之前先确认服务端能看到：

.. code-block:: bash

   curl -s http://YOUR_PC:8080/api/catalog | head -c 400

4. 装到 Hub 上
--------------

在 Web UI（ ``http://HUB/`` ）中打开 ``商店``，将自定义商店 URL 设为
``http://YOUR_PC:8080``，然后刷新目录并安装 ``my_js``。

5. 看到它跑起来
---------------

Web 登录默认开启，写操作需要会话。第 4 步的安装已经把模块装好并顺带重载了 Hub，
这里只差一个会话：先设地址、登录一次，之后复用 Cookie：

.. code-block:: bash

   export HUB=<hub-ip>                              # 来自 eth got IP 日志
   curl -s -c /tmp/mh -X POST http://$HUB/api/auth/login \
     -H 'Content-Type: application/json' \
     -d '{"username":"admin","password":"secret1"}'
   curl -s -b /tmp/mh http://$HUB/api/points | grep my_js  # 你的点已登记
   curl -s -b /tmp/mh -X POST http://$HUB/api/command \
     -H 'Content-Type: application/json' \
     -d '{"point_id":"my_js.lamp","cmd":"turn_on"}'

打开控制台：概览页出现这台设备，并且是标准灯卡片，串口日志里有 ``my_js ready``。
控制走的就是你刚发的 ``POST /api/command`` —— 也正是规则引擎调用的那条路径，所以
插件第一天就能被自动化。

6. 改它
-------

改 ``main.js``，把 ``module.json`` 里的版本号加一位，重复第 3 步，然后：

.. code-block:: bash

   curl -s -b /tmp/mh -X POST http://$HUB/api/store/upgrade \
     -H 'Content-Type: application/json' \
     -d '{"type":"module","id":"my_js"}'

不重编 Host，不等整包 OTA。 ``/api/store/upgrade`` 装完就顺带重载一次（想跳过就传
``"reload": false`` ）；重载会重跑 ``setup`` 并清掉该模块的定时器，所以卡死的回调
不会活过一次 reload。

没有出来时查这些
----------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - 现象
     - 查什么
   * - ``/api/catalog`` 返回 ``no_plugin_for_version``
     - ``targets`` 与 Hub 的 ``?chip=`` （ ``esp32s31``）不匹配，或包没和它的
       ``catalog.json`` 放在同一目录
   * - 安装成功但没有点
     - 加载器按 ABI 拒绝：比对 ``module.json`` 的 ``abi_version`` /
       ``core_api_min`` / ``core_api_max`` 与 ``GET /api/`` 的 ``api_version``
   * - 日志说模块被 ``load guard`` 跳过
     - 上次加载崩溃过；用
       ``POST /api/modules {"id":"my_js","clear_load_guard":true}`` 清除，并先看串口
       （:doc:`../faq`）
   * - 浏览器控制台记 ``type not in catalog``
     - ``type`` 不在类型目录里 —— 是数据问题，不是渲染问题（ :doc:`../type_catalog` ）
   * - 插件在跑时界面不响应
     - 某个 JS 回调占住了共享 worker；把活挪到 ``host.adapter.poll`` / ``setTimeout``

换到另一层
----------

L1 是同一个循环，只把第 2 步换成在 ``module.json`` 里写 ``inject``：没有代码、
没有 ``setup``，而且只能给 Host 已有的适配器补默认参数（见 :doc:`develop_l1` ）。

.. only:: not hide_l3

   L3 把第 2 步换成 C 源文件，并在打包前多一步编译：先产出 ``.so``，用
   ``python3 tools/build_plugin_standalone.py``，或者与 Host 一起编译时改用
   ``./tools/build_firmware.py modules``。MHMD 头是另一条命令套上去的：
   ``pack_store_module.py mhmd in.so out.so`` —— ``pack`` 只负责把已经封装好的
   ``.so`` 装进包里（见 :doc:`develop_l3` ）。

每步的完整细节：:doc:`develop_l2`、:doc:`pack_and_catalog`、:doc:`../api/host_rest`。

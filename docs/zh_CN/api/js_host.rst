JS 插件 Host API
================

:link_to_translation:`en:[English]`

概述
----

JS 插件是跑在 Hub 里的 JavaScript，其 ``module.json`` 以 ``runtime: js`` 标记。
它通过 QuickJS 绑定访问 Host 能力，不靠链接器。这些绑定实现在
``components/minihub_js/``，由 ``CONFIG_MINIHUB_JS_PLUGINS`` 决定是否编入。

REST 安装与 RPC 见 :doc:`plugin_rest`。C 侧点位/命令模型见 :doc:`minihub_core`。

.. only:: not hide_l3

   ELF 插件使用 :doc:`module_abi` 中的虚表。

host 对象
---------

``setup(host)`` 拿到的就是下表，构建时直接从 QuickJS 绑定里读出来，所以改名或删掉的
方法不会继续留在本页。斜杠后的数字是 C 侧登记的参数个数；表按命名空间列出各自的方法。

.. minihub-JS-api::

模块入口
--------

Hub 在模块加载及每次 ``POST /api/hub/reload`` 后对 enabled 的 JS 模块调用
``setup(host)``。``main.js`` 必须定义 ``setup``；缺失则打 warning 并跳过模块。
每个模块的顶层名字只留在这个模块里。

在 ``setup`` 中为模块 id 使用独立 ``point_id`` 前缀；重载时会再次调用，避免重复注册。
**一台设备登记一条点** （``模块.设备``），不要按参数再拆点。

实现
----

* ``components/minihub_js/minihub_js.c`` —— 核心绑定（log、point、command、
  event、http、config、module、timer、adapter、time）与定时器全局函数。
* ``components/minihub_js/minihub_js_net.c`` / ``minihub_js_crypto.c`` —— UDP 与
  摘要/HMAC/AES 助手。
* ``components/minihub_js/minihub_js_ble.c`` —— 共享 BLE 广播扫描器。
* ``components/minihub_js/minihub_js_mdns.c`` —— 一次性局域网服务发现。
* ``components/minihub_js/minihub_js_mqtt.c`` —— 按所有者的 MQTT 发现客户端。

入口函数
--------

setup(host)
~~~~~~~~~~~

.. code-block:: javascript

   function setup(host) { ... }

Hub 注入的能力对象；见下文各 ``host.*`` 字段。

参数
^^^^

``host`` (object)
    Host 注入的能力对象。

返回
^^^^

``undefined``。

按源文件分页
------------

下面的参考按 C 源文件一页一个，与构建结构对应。

.. toctree::
   :maxdepth: 1

   js_host_core
   js_host_net
   js_host_ble
   js_host_mdns
   js_host_mqtt

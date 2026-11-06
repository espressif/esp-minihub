错误码总表
==========

:link_to_translation:`en:[English]`

出问题时 Hub 可能回什么，全部在构建期从固件源码里读出来。清单本身写在
``docs/_extensions/minihub_errors.py`` 里：它扫描所有会上报 HTTP 错误的文件，以及
MCP 的两份源文件，所以新增一条错误会自动进表。反过来，如果代码开始返回指南叫不出
名字的东西，或者表里留着一条代码已经不再返回的旧记录，文档构建会直接失败，而不是
把过时的表发出去。

用这一页的方式是从另一端入手：你手上只有一个状态码、一段正文、或一个 ``code``，
想知道谁会发它。如果你想顺着现象往下查，看 :doc:`../troubleshooting`。

HTTP 状态
---------

REST 调用失败时没有 JSON。 ``httpd_resp_send_err()`` 回的是 ``text/plain``，正文就是这个
处理函数对失败最简短的说法。

.. minihub-errors:: http

其中三个状态码还带状态码本身看不出的行为：

* **401** 的响应里同时清掉 ``mh_sess`` cookie，因此一直开着的控制台会退回登录页，
  而不是拿一个已经失效的会话继续重试。它的正文永远是 ``unauthorized``；只有一个例外：
  改密码时旧密码填错，回的还是 **401**，正文是 ``wrong old password``。
* **500** 的正文要么是 ``oom`` / ``no mem``（内存分配失败），要么是被调用方返回的
  ``esp_err`` 名字，所以线上看到 ``ESP_ERR_TIMEOUT`` 就是 Hub 内部真的超时了，
  不是一个笼统的失败。
* **409** 只用于“位置已被占用”：同一时间只能跑一个 OTA，初始密码只能设一次。

还有一批响应是 HTTP 服务器本身在处理任何 MiniHub 处理函数之前就发出的，表里没有它们，
因为 MiniHub 源码不会主动产生：URI 从未注册过是 **404**，URI 注册了但方法不对是 **405**，
URI 长过服务器缓冲是 **414**，连接建好却一个请求都没发是 **408**，HTTP 版本不是
1.0/1.1 是 **505**，而服务器算不出长度的 body 会给 **501** —— 服务器不接受
``Transfer-Encoding``，所以流式发 body 的客户端（ ``curl -T -``，以及某些 HTTP 库对
文件句柄的默认处理）在载荷读到之前就已被拒绝。发 body 请带上已知长度。

错误正文
--------

固件里逐字写进错误响应的每一个字符串，以及会返回它的端点。当你手上只有 curl
打出来的那一行时，要查的就是这张表。

.. minihub-errors:: messages

* ``invalid JSON`` 与 ``empty body`` 最常见，两者都说明请求还没走到真正的操作。
  ``empty body`` 是字面意思：处理函数读到 0 字节。 ``invalid JSON`` 有两个原因—— body
  本身不是 JSON，或者它比那个处理函数恰好预留的缓冲区更长。读进定长数组的端点
  （ ``/api/store/install`` 只收 255 字节）会把 body 截断，然后解析失败；走
  ``read_http_body()`` 的端点则提前拒收超长 body，回 ``invalid body`` 或
  ``body too large``。请求头从来不是问题：没有任何处理函数看 ``Content-Type``。
* ``missing <field>`` 说出的是处理函数找了却没找到的那个键。每个端点需要哪些字段，
  见 :doc:`host_rest` 与 :doc:`plugin_rest`。
* 最后一列给的是函数名而不是 URI 时，这个回复是在工作线程或共享辅助函数里发出的
  （ ``preview_async_worker`` 、 ``rules_import_apply_root`` ），没有一个端点独占它。
* 运行时由 ``esp_err_to_name()`` 拼出来的正文无法列在这里—— 它可以是任何 ``ESP_*``
  名字，而状态码已经告诉你该读表里的哪一行。

MCP 错误码
----------

端上 MCP 服务器把失败放在**同一个正常的工具结果里**：JSON-RPC 调用成功，返回的字符串
是 ``{"ok":false,"error":"CODE"}``。只看协议层是否报错的客户端，会看到一次绿色调用配上
一次失败的操作。

.. minihub-errors:: mcp

``code`` 出自两个辅助函数之一，而选哪一个决定了结果里有多少信息。 ``hub.api`` 用
``err_obj()`` 构造回复，它会多带一个 ``detail``—— 网关自己的一句话，或
``save_adapters`` 这样的阶段名；只有传 ``NULL`` 的那一处例外，你只能拿到裸 code。
其余工具都走 ``mcp_error()``，只回 code，所以 code 就是全部信息：

* ``HUB_NOT_READY``—— 一次重载正在进行。等一会儿重试； ``hub.reload`` 被限制为 5 秒
  一次，而 REST 的 ``POST /api/hub/reload`` 没有这个限制。
* ``DENIED``—— 方法或路径不在 ``hub.api`` 白名单内，逐条规则见 :doc:`mcp`。

``err_obj()`` 也会把 ``esp_err_to_name(err)`` 当 code 传出去，所以一次网关失败可能回
``ESP_ERR_NOT_FOUND`` 或 ``ESP_FAIL``，而上表按类别列的是另外一批名字。它们不是第二套
体系：你要找的就是 gloss 里写明“ detail 会说明”的那一行。

相关章节
--------

* :doc:`host_rest` 、 :doc:`plugin_rest`——状态码背后的那些端点
* :doc:`mcp`——工具清单与 JSON-RPC 外层结构
* :doc:`../troubleshooting`——从现象到码，按顺序
* :doc:`../limits`—— ``oom`` 与 ``worker busy`` 来自哪些容量上限

Hub 固件升级
============

:link_to_translation:`en:[English]`

这里升级的是 **Hub 应用** 本身。升级终端设备走另一条路（:doc:`radios`、
:doc:`api/host_rest`）。槽位规划见 :doc:`production`，产品目录与槽大小见
:doc:`host_customization`。

App OTA **不会** 替换 ``/data/web/`` 下的控制台；控制台升级是单独一步，见
:doc:`web_console`。恢复出厂与整机 ``flash`` 见 :doc:`production`。

工作原理
--------

OTA 的双槽写入、重启切换与回滚由 ESP-IDF 实现，原理见
`ESP-IDF OTA 升级文档
<https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/system/ota.html>`_。
在 minihub 上，你只负责 **触发升级并观察进度**，其余交给 ESP-IDF。

.. image:: ../_static/ota_update/update_flow.svg
   :align: center
   :alt: MiniHub 固件更新：读提示 → 后台重探 → 交 URL → 写空闲槽 → 校验 → 换槽重启并确认

`交互版 <_static/ota_update/index.html>`_ —— 卡片自动逐站导览（悬停暂停），青色回程线就是回滚路径。

镜像必须能装进槽内。Matter 构建需要更大的槽；当链接器提示应用放不下时，请放大
``ota_0`` / ``ota_1`` （见 :doc:`production` 与 :doc:`host_customization` ）。

从 OTA 服务器拉取
-----------------

在 Web UI 中手动刷新并触发：进入 **系统** 页后，Hub 会向已配置的 **OTA 服务器
（部署根）** 查询本机芯片是否有可用更新；有更新时固件版本一行显示「新版本 vX」徽标，
点 **更新固件** 即从该服务器拉取固件并刷写。这条路径 **不** 经过插件商店，也 **不** 走
手动 URL OTA（ ``POST /api/ota`` ）那一条。

Web UI 操作：

1. 打开 ``http://HUB/``，进入 **系统** 页。
2. 固件版本一行若显示「新版本 vX」徽标，说明 OTA 服务器上有适配本机芯片的更新；
   点徽标可看发布说明，点 **更新固件** 即开始拉取并刷写。
3. 进度显示在 OTA 状态条；完成后设备自动重启，版本号随之更新。

Hub 只会拉取匹配当前芯片的固件，选出版本号高于 ``CONFIG_MINIHUB_VERSION`` 的最高
一项。下载下来的固件 **不做摘要校验**：写完之后由 ESP-IDF 的 ``esp_ota_end`` 校验
镜像头与序列号校验和，而部署索引里的 ``sha256`` 只被记录下来，用来判断「同一个版本号
但重新构建过」要不要重刷。真正按 ``sha256`` 校验的是控制台与资源文件的下载。部署
索引的字段与版本判定逻辑见 :doc:`api/deploy_index`；这条路径不读取 ``/api/catalog``
的 ``firmware_upgrade`` 字段（该字段只报告本芯片已发布的最高版本）。

抓取只跑一次：不续传、不重试，HTTP 非 200 或读取卡死都会记成失败。写入目标是另一个
应用槽 —— 出厂的 16 MB 布局里是 5 MiB，所以镜像过大不会提前拒绝，而是写到一半失败
（见 :doc:`production`）。

下面的 URL OTA 与上传 OTA 是手动路径，也可在 Web UI 的 **系统** 页用固件 URL 或
选择 ``.bin`` 文件触发。设备不检查 URL 的 scheme，实验室内网用 ``http`` 也能刷；
只要是第三方可达的地址就用 **HTTPS** —— 证书校验依赖编译进来的信任根
（``CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`` ），默认构建是开的 （上行见
:doc:`networking` ）。

查询状态
--------

Web UI 的 OTA 状态条已实时显示进度；REST 客户端可用 ``GET /api/ota`` 轮询。

``GET /api/ota`` 返回 JSON：

* ``state`` — ``0`` 空闲、 ``1`` 进行中、 ``2`` 成功、 ``3`` 失败 （是数字，不是字符串）
* ``error`` — 最近错误字符串（无错误时为空），最长保留 95 个字符
* ``progress`` — 0–100，未知时为 ``-1``
* ``bytes_written`` — 已写入升级槽的字节数
* ``source`` — ``none``、 ``url``、 ``upload`` 或 ``rainmaker``

``2`` 很容易错过：写成功后设备约 800 ms 就重启，轮询方通常看到的是 ``1``，再连上时
已经是新机上的 ``0``。确认落在哪个槽请查 ``GET /api/system`` 里的版本。

URL OTA
-------

Web UI：在 **系统** 页的固件更新区填入固件 URL，点 **开始更新**。

也可走 REST（辅助）： ``POST /api/ota``，JSON 请求体， **HTTP 200** 已启动；
已有 OTA 在跑则 **409**，响应体是纯文本 ``busy``：

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://..."}'

成功响应： ``{"result":"started"}``。用 ``GET /api/ota`` 查进度。

你传进去的 URL 有两点要知道：

* 请求体只读进 256 字节的缓冲，算上引号与键名，URL 大约只剩 245 个字符。更长的
  请求体被截断后 JSON 解析失败，回 **400** ``invalid json``。
* 设备会在 URL 上追加 ``mac=``、``chip=``、``cur_version=`` 再发起下载。按设备灰度
  时很方便，但 query 串被改动过 —— 预签名或有有效期的 CDN URL 会因此签名不符。要么
  用忽略多余 query 的路径，要么改走部署服务器拉取那条。

上传 OTA
--------

这是直连设备的推送路径：直接连上设备（例如在其局域网访问 ``http://HUB/`` ），把固件
``.bin`` 直接推送给它，不经过 OTA 服务器。

Web UI：在 **系统** 页的固件更新区选择 ``.bin`` 文件，点 **上传**；写完后设备
重启。该路径不在 MCP ``hub.api`` 白名单，走 Web UI 或局域网客户端即可，不要走
``hub.api``。

也可走 REST（辅助）： ``POST /api/ota/upload`` 接受原始 ``.bin`` 请求体，必须带
``Content-Length``：

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @minihub.bin

写完镜像后 **HTTP 200** ``{"result":"ok"}`` （设备随后重启）。忙则 **409**。
缺少 ``Content-Length`` 为 **400**。

启动与回滚
----------

出厂构建开了 ``CONFIG_BOOTLOADER_APP_ROLLBACK``，并选择“启动时确认”：新固件由
ESP-IDF 在 ``app_main`` 之前自动标记为有效。所以回滚只覆盖 **完全起不来** 的固件 ——
新槽还没被确认时，引导加载程序会退回另一个槽。

应用层没有自检窗口，也没有接受/拒绝固件的 REST 接口：新固件一旦跑到 ``app_main``
就被固化，之后即便行为不对也不会自动回退。要退回旧版，用上面的上传或 URL 路径推一个
已知可用的构建（写入应用槽时不比较版本号），或者用 UART 下载做整机 ``flash``
（见 :doc:`production`）。

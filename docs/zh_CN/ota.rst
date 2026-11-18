Hub 固件升级
============

:link_to_translation:`en:[English]`

这里升级的是 **Hub 应用** 本身。升级终端设备走另一条路（ :doc:`radios` 、
:doc:`api/host_rest` ）。槽位规划见 :doc:`production`，产品目录与槽大小见
:doc:`host_customization`。

App OTA **不会** 替换 ``/data/web/`` 下的控制台；控制台升级是单独一步，见
:doc:`web_console`。恢复出厂与整机 ``flash`` 见 :doc:`production`。

工作原理
------------

Hub 始终从 ``ota_0`` 运行。另一个应用分区是 ``vibe_mode`` （ ``app,factory`` ），
存放常驻的维护镜像。只有它能写应用槽。

1. URL、上传与部署服务器三条更新路径，都把裸 OTA ``.bin`` 下载到外部 LittleFS
   卷的 ``/Firmware/firmware.bin``。运行中的固件不写 flash。
2. 设备把所用介质记入 NVS（ ``minihub/vibe_dev`` ），对暂存文件计算哈希，在同目录
   写入 ``firmware.bin.sha256`` ，把 SHA 存入 ``minihub/vibe_sha`` ，一次性置位
   ``minihub/vibe_pending`` ，然后选中 ``vibe_mode`` 。ESP-IDF 对
   ``app,factory`` 目标的处理是擦除 ``otadata`` ，所以下次启动进入 Vibe Mode。
3. 仅当 ``vibe_pending`` 置位，或 ``CONFIG_MINIHUB_VIBE_FORCE_GPIO`` 按键按下时
   （ ``-1`` 表示不检查按键）才会搬运。写 flash 前读取 staged 镜像的
   ``esp_app_desc`` 并与 ``ota_0`` 比对：``project_name`` 不一致则拒绝（例如
   ``minihub`` 不得覆盖 ``minihub_lcd``；``ota_0`` 无有效描述符时不做该项检查）；
   ``app_elf_sha256`` 相同则跳过搬运。需要搬运时重算文件 SHA，并与 NVS 中的期望
   值比对（pending 路径缺 NVS 时回退 sidecar；仅按键强刷时只对照 sidecar），不匹配
   则拒绝。
4. 需要搬运时以 ``esp_ota_begin()`` / ``esp_ota_write()`` / ``esp_ota_end()``
   写入 ``ota_0``。``esp_ota_end()`` 校验镜像。写入未完成即断电会留下无效槽，下次
   启动重新搬运。
5. Vibe Mode 重写 ``otadata`` 选中 ``ota_0`` 并重启。没有镜像、未武装或搬运失败时
   同样执行这一步；否则引导加载程序每次都会再次选中 factory 应用。

外部介质
------------

SDMMC 卡或 USB 大容量存储设备上是一个 LittleFS 卷，挂载点为 ``/vibe`` 。它不是
FAT，PC 不会把它当普通盘挂载；下载时由 Hub 自己写入镜像。
``CONFIG_MINIHUB_VIBE_*`` 决定探测哪些设备，以及 SDMMC 的总线宽度与 GPIO。
首次挂载成功后， ``minihub/vibe_dev`` 保存 ``sdmmc`` 或 ``usb`` 。该键缺席时，
Vibe Mode 会依次探测两种设备。

``minihub/vibe_result`` 保存最近一次交接结果（ ``copied`` 、 ``unchanged`` 、
``copy failed`` 、 ``no firmware`` 、 ``no media`` ）。搬运失败会在下次启动重试，
最多三次；之后 Vibe Mode 停在自身运行，等待更换介质。

新镜像必须能放进 ``ota_0``。过大的镜像在擦除 flash 之前就会被拒绝
（见 :doc:`production` ）。

从 OTA 服务器拉取
--------------------

在 Web UI 中手动刷新并触发：进入 ``系统`` 页后，Hub 会向已配置的 OTA 服务器
（部署根）查询本机芯片是否有可用更新；有更新时固件版本一行显示「新版本 vX」徽标，
点 ``更新固件`` 即下载并暂存该镜像。这条路径 **不** 经过插件商店，也 **不** 走
手动 URL OTA（ ``POST /api/ota`` ）那一条。

Web UI 操作：

1. 打开 ``http://HUB/`` ，进入 ``系统`` 页。
2. 固件版本一行若显示「新版本 vX」徽标，说明 OTA 服务器上有适配本机芯片的更新；
   点徽标可看发布说明，点 ``更新固件`` 即开始下载。
3. 进度显示在 OTA 状态条。暂存完成后设备先重启进入 Vibe Mode，再重启进入更新后的
   ``ota_0`` ，版本号随之更新。

Hub 只会拉取匹配当前芯片的固件，选出版本号高于 ``CONFIG_MINIHUB_VERSION`` 的最高
一项。下载内容 **不做摘要校验**：部署索引里的 ``sha256`` 只被记录下来，用来判断
「同一个版本号但重新构建过」要不要重新拉取。Vibe Mode 搬运时用 ``esp_ota_end()``
校验镜像；若 staged 与 ``ota_0`` 的 ``app_elf_sha256`` 相同则跳过搬运。控制台与
资源文件的下载才会按 ``sha256`` 校验后替换。部署索引的字段与版本判定逻辑见
:doc:`api/deploy_index`；这条路径不读取 ``/api/catalog`` 的 ``firmware_upgrade``
字段（该字段只报告本芯片已发布的最高版本）。

抓取只跑一次：不续传、不重试，HTTP 非 200 或读取卡死都会记成失败。

下面的 URL OTA 与上传 OTA 是手动路径，也可在 Web UI 的 ``系统`` 页用固件 URL 或
选择 ``.bin`` 文件触发。设备不检查 URL 的 scheme，实验室内网用 ``http`` 也能刷；
只要是第三方可达的地址就用 HTTPS —— 证书校验依赖编译进来的信任根
（ ``CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`` ），默认构建是开的 （上行见
:doc:`networking` ）。

查询状态
------------

Web UI 的 OTA 状态条已实时显示进度；REST 客户端可用 ``GET /api/ota`` 轮询。

``GET /api/ota`` 返回 JSON：

* ``state`` — ``0`` 空闲、 ``1`` 进行中、 ``2`` 成功、 ``3`` 失败 （是数字，不是字符串）
* ``error`` — 最近错误字符串（无错误时为空），最长保留 95 个字符
* ``progress`` — 0–100，未知时为 ``-1``
* ``bytes_written`` — 已暂存到外部卷的字节数
* ``source`` — ``none`` 、 ``url`` 、 ``upload`` 或 ``rainmaker``

``2`` 表示镜像已暂存、交接 Vibe Mode 已开始。设备约 800 ms 后重启，客户端通常只在
更新后的固件上看到 ``state`` 回到 ``0``。查运行中版本请轮询 ``GET /api/system``。

URL OTA
------------

Web UI：在 ``系统`` 页的固件更新区填入固件 URL，点 ``开始更新``。

也可走 REST（辅助）： ``POST /api/ota`` ，JSON 请求体， **HTTP 200** 已启动；
已有 OTA 在跑则 **409** ，响应体是纯文本 ``busy`` ：

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://..."}'

成功响应： ``{"result":"started"}`` 。用 ``GET /api/ota`` 查进度。

你传进去的 URL 有两点要知道：

* 请求体只读进 256 字节的缓冲，算上引号与键名，URL 大约只剩 245 个字符。更长的
  请求体被截断后 JSON 解析失败，回 **400** ``invalid JSON`` 。
* 设备会在 URL 上追加 ``mac=`` 、 ``chip=`` 、 ``cur_version=`` 再发起下载。按设备灰度
  时很方便，但 query 串被改动过 —— 预签名或有有效期的 CDN URL 会因此签名不符。要么
  用忽略多余 query 的路径，要么改走部署服务器拉取那条。

上传 OTA
------------

这是直连设备的推送路径：直接连上设备（例如在其局域网访问 ``http://HUB/`` ），把固件
``.bin`` 直接推送给它，不经过 OTA 服务器。

Web UI：在 ``系统`` 页的固件更新区选择 ``.bin`` 文件，点 ``上传`` ；暂存完成后设备
重启。该路径不在 MCP ``hub.api`` 白名单，走 Web UI 或局域网客户端即可，不要走
``hub.api``。

也可走 REST（辅助）： ``POST /api/ota/upload`` 接受原始 ``.bin`` 请求体，必须带
``Content-Length`` ：

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @minihub.bin

镜像暂存完成后 **HTTP 200** ``{"result":"ok"}`` （设备随后重启）。忙则 **409** 。
缺少 ``Content-Length`` 为 **400** 。

启动与恢复
------------

这里没有 A/B 应用槽对，也没有 ESP-IDF 的 OTA 回滚：那需要两个应用槽，而 ``ota_0``
是唯一的运行槽。恢复由 Vibe Mode 承担。搬运途中断电会让 ``ota_0`` 不再是有效镜像，
引导加载程序退回 factory 应用，Vibe Mode 重新搬运暂存的镜像。

单槽仍开启 ``CONFIG_BOOTLOADER_APP_ROLLBACK`` 与「启动时确认」，
``minihub_ota_init()`` 在交接后的首次启动确认应用。应用层没有自检窗口，也没有
接受/拒绝固件的 REST 接口：新固件一旦跑到 ``app_main`` 就被固化。要退回旧版，用
上传或 URL 路径暂存一个已知可用的构建，或者用 UART 下载做整机 ``flash``
（见 :doc:`production` ）。

RainMaker 云 OTA 会被拒绝：它的默认回调写入备用 OTA 槽，而该槽已不存在。请改用
URL、上传或部署服务器拉取路径。

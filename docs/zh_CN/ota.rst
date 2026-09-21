Hub 固件升级
============

:link_to_translation:`en:[English]`

现场 **Hub 应用** 固件升级（运维）。终端设备 OTA 见 :doc:`radios` 与
:doc:`api/host_rest`。双 app 槽 ``ota_0`` / ``ota_1`` 与工厂分区规划见
:doc:`production` 。OEM 产品目录与槽大小见 :doc:`host_customization` 。REST 总览见
:doc:`api/host_rest` 。

NVS 恢复出厂（按住配置的 GPIO）**不会** 清空 LittleFS ``/data`` 与 ``/store`` 。
工装需要干净文件系统时请整机 ``flash`` 。App OTA 同样 **不会** 替换
``/store/preinstall/web/`` 上的控制台，见 :doc:`web_console` 。

槽位与镜像大小
--------------

下载写入空闲槽，再重启切过去。镜像必须装得进槽。Matter 构建需要大槽；链接报装不下时
放大 ``ota_0`` / ``ota_1`` （见 :doc:`production` 与 :doc:`host_customization` ）。

URL OTA 须使用设备能访问的 **HTTPS** （上行见 :doc:`networking` ）。

查询状态
--------

``GET /api/ota`` 返回 JSON：

* ``state`` — ``0`` 空闲、 ``1`` 进行中、 ``2`` 成功、 ``3`` 失败
* ``error`` — 最近错误字符串（无错误时为空）
* ``progress`` — 0–100，未知时为 ``-1``
* ``bytes_written`` — 已写入升级槽的字节数
* ``source`` — ``none`` 、 ``url`` 、 ``upload`` 或 ``rainmaker``

URL OTA
-------

``POST /api/ota`` ，JSON 请求体。 **HTTP 200** 表示已启动；已有 OTA 进行中则
**409** 。

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://..."}'

成功响应： ``{"result":"started"}`` 。用 ``GET /api/ota`` 查进度。

上传 OTA
--------

``POST /api/ota/upload`` 接受原始 ``.bin`` 请求体。必须带 ``Content-Length`` 。
该路径 **不在** MCP ``hub.api`` 白名单；请用 REST（或局域网客户端），不要走
``hub.api`` 。

.. code-block:: bash

   curl -s -X POST http://HUB/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @minihub.bin

写完镜像后 **HTTP 200** ``{"result":"ok"}`` （设备随后重启）。忙则 **409** 。
缺少 ``Content-Length`` 为 **400** 。

启动与回滚
----------

新槽以待验证状态启动。``minihub_ota_init`` 随后调用
``esp_ota_mark_app_valid_cancel_rollback`` 。若这次启动跑到了 OTA 初始化，新固件留下。
新固件若根本起不来，bootloader 回到上一槽。没有 REST 接口可以接受或拒绝该镜像。
两槽都起不来时，用 UART 下载做整机 ``flash`` 恢复。

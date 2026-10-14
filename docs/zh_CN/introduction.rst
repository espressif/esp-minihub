简介
========

:link_to_translation:`en:[English]`

MiniHub 是乐鑫 ESP32-S31 等平台上的多协议物联网 Host 固件：Web UI、REST、
Store 动态模块（mhub 包），以及 Zigbee / BLE Mesh / Matter / MQTT / Modbus 等适配器。

通用 Host 不绑定单一生态。乐鑫维护的协议栈与 Hub 基础能力留在 Core。厂商云与
OEM 差异有两种交付方式：以 Store 插件交付（可按 SKU 预装，或在 ABI 匹配时现场
安装升级），无需维护 Host 代码；或基于 Host 二次开发编入产品固件，更节约内存，
并可更灵活地调用 Plugin SDK 与 ESP-IDF API。自动化层统一使用点位：
**一台设备 = 一条 point** （``point_id`` 如 ``sim.js_kitchen``），实时参数在
``props.params``，而不是各家的私有 API。类型文案与控件来自 Host 类型目录，不在
Core C 里。

仓库中，``application/APP/`` 表示产品（板级、分区、出厂配置），``components/`` 为
共享 Core 与适配器。产品目录见 :doc:`application/index`。本指南以 manifest、JSON、REST/MCP 与模块 ABI 为二次开发契约，
无需阅读 Core C。项目以 Apache 2.0 发布，协作规范见 :doc:`contributing`。

首次编译烧录见 :doc:`getting_started`。分层与热重载见 :doc:`architecture`；
点位与 Hub 配置见 :doc:`data_model`；设备类型与 UI 见 :doc:`type_catalog`；
各射频如何出点、认领与 OTA 见 :doc:`radios`。

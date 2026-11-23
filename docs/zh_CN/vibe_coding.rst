Vibe coding
===========

:link_to_translation:`en:[English]`

MiniHub 可用 AI 编程助手开发：``application/`` 下的 OEM 产品、商店插件，或一台
已在局域网以 REST 应答的 Hub。助手是可选加速器，不是前提；建议先手工确认同一流程
可复现，再交由 agent 重复。下文说明如何配置。

让 agent 从哪里读说明
---------------------

把仓库根目录作为工作区打开。agent 依据的是下面这些文件，也可以直接把其中任何一个
交给它：

* ``AGENTS.md`` （仓库根） — 所有助手的入口。里面有文档地图、Git 规则、Host 约束与
  核心数据模型；与 IDE 专属配置冲突时以它为准。
* ``.agent/skills/<name>/SKILL.md`` — 一类任务一个项目 skill。改 Host 代码、写商店
  插件或改本指南之前，先让 agent 读对应的 skill： ``minihub-host`` （产品、Kconfig、
  协议栈）、 ``minihub-docs`` （本指南），每个插件层级另有一个
  ``minihub-plugin-*`` skill。
* ``docs/en/`` 与 ``docs/zh_CN/`` — 本指南。

``.cursor/skills`` 是指向 ``.agent/skills/`` 的符号链接，所以只认自己目录的工具读到的
仍是同一批文件。

把章节交给它
------------

把任务对应的章节交给 agent。二次开发落在产品或插件里 —— 从那里读起，不要先打开
Core 的 ``.c``：

* 架构与点位： :doc:`architecture`、 :doc:`type_catalog`、 :doc:`data_model`
  （一台设备一条点；控件看类型目录 CSV）
* 射频与接入： :doc:`radios`
* 编译烧录： :doc:`getting_started`
* OEM Host： :doc:`host_customization`、 :doc:`production`
* 商店插件： :doc:`plugins/overview` （先选 JSON 或 JS，再进对应章）
* REST / MCP 工具列表： :doc:`api/host_rest`、 :doc:`api/mcp`
* 贡献与 Git： :doc:`contributing`

助手应遵守
----------

* Host Core 保持生态中立：不要把厂商云 token、默认适配器或厂商规则写进
  ``components/minihub_*`` 或通用 ``configs/``
* 第三方 / 演示插件放 ``examples/plugins/ID/`` 或 ``MINIHUB_PLUGIN_DIRS``；打成 ``.mhub``

IDE MCP
-------

写插件另带一个 MCP 服务。仓库把 Cursor （``.cursor/mcp.json``）与 VS Code
（``.vscode/mcp.json``）指向 ``python3 tools/minihub_plugin_mcp.py``，提供
``plugin_guide``、 ``scaffold_plugin``、 ``validate_plugin``、 ``pack_plugin``；
配置文件本身在 ``.agent/mcp/``，由符号链接进入这两个目录。它跑在你的电脑上，面向
源码树。下面说的设备端服务是另一回事：它经网络访问 Hub。

设备端 MCP
----------

Hub 自己也能提供 MCP（ ``CONFIG_MINIHUB_MCP_SERVER`` 默认打开）。初始化
``components/esp-claw`` 子模块；STA / 以太网 / SoftAP 任一拿到 IPv4 后启动 HTTP
MCP。地址 ``http://HUB_IP:18791/mcp`` 或
``http://esp-minihub.local:18791/mcp`` （mDNS 实例 ``ESP-MiniHub``，类型
``_mcp._tcp``）。步骤与工具列表见 :doc:`api/mcp`。

启动或 ``hub.reload`` 过程中调用可能得到 ``HUB_NOT_READY``；``hub.reload`` 限速
5 秒一次。不用 MCP 时走 ``http://HUB/api/`` （见 :doc:`api/host_rest` ）。

工作方式建议
------------

常见做法：agent 先读对应的 skill 与任务章节，再改代码，然后在设备上验证 ——
串口日志、``GET /api/modules``，必要时 ``POST /api/hub/reload``。

.. only:: not hide_l3

   ELF 未定义符号须在 ``host_export_allowlist.json`` 内，声明来自
   ``minihub_plugin_sdk.h`` — :doc:`plugins/develop_elf`。

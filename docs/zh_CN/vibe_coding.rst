Vibe coding
===========

:link_to_translation:`en:[English]`

用 AI 编程助手（IDE Agent、终端 Agent、MCP 客户端）二次开发 MiniHub：改 Host
产品、写 Store 插件、或通过设备端 MCP 操作运行中的 Hub。

约定以本编程指南与公开头文件为准。把本仓库作为工作区打开即可，助手会读根目录
``AGENTS.md`` ；不必再单独配置。若与 ``AGENTS.md`` 冲突，以本指南为准。

让助手读对文档
--------------

把仓库根目录交给助手，并指定任务对应的章节，避免它去翻 Core ``.c`` ：

* 架构与点位： :doc:`architecture` 、 :doc:`type_catalog` 、 :doc:`data_model`
  （一台设备一条点；控件看类型目录 CSV）
* 射频与接入： :doc:`radios`
* 编译烧录： :doc:`getting_started`
* OEM Host： :doc:`host_customization` 、 :doc:`production`
* Store 插件： :doc:`plugins/overview` （L1 / L2 / L3 选型后再进对应章）
* REST / MCP 工具列表： :doc:`api/host_rest` 、 :doc:`api/mcp`
* 贡献与 Git： :doc:`contributing`

助手应遵守
----------

* Host Core 保持生态中立：不要把厂商云 token、默认适配器或厂商规则写进
  ``components/minihub_*`` 或通用 ``config/``
* 厂商 / 演示插件放 ``components/minihub_plugins/ID/`` 或
  ``examples/plugins/ID/`` + ``MINIHUB_PLUGIN_DIRS``，打成 ``.mhub``
* 分支名用下划线、不用连字符（如 ``feature/modbus_tcp_client`` ）
* 提交作者为人类 ``user.name`` / ``user.email`` ；不要加 AI 共著 trailer

IDE MCP
-------

写 Store 插件时，本仓库已配置 Cursor ``.cursor/mcp.json`` 与 VS Code
``.vscode/mcp.json`` ，启动 ``python3 tools/minihub_plugin_mcp.py``
（工具： ``plugin_guide`` 、 ``scaffold_plugin`` 、 ``validate_plugin`` 、
``pack_plugin`` ）。这是给 IDE 用的，不是下面的设备端 MCP。

设备端 MCP
----------

可选。打开 ``CONFIG_MINIHUB_MCP_SERVER`` （依赖 ``components/esp-claw`` 子模块）。
本地 MCP 客户端经产品接线的 MCP 传输访问 Hub，工具返回 JSON 文本。

多数工具要求 Hub 已就绪；启动或 ``hub.reload`` 期间可能 ``HUB_NOT_READY`` 。
MCP ``hub.reload`` 限速 5 秒。完整工具与 ``hub.api`` 白名单见 :doc:`api/mcp` 。

典型用法：

* ``points.list`` / ``command.invoke`` — 查点位、下发命令
* ``hub.api`` — 代理白名单 REST（JSON GET/POST/PUT；不能传 multipart 固件）
* ``store.config.*`` / 安装类工具 — 改 Store URL、装模块（见 REST 章）

没有 MCP 时，助手仍可用局域网 ``http://HUB/api/`` （见 :doc:`api/host_rest` ）。

工作方式建议
------------

1. 先让助手读本章与任务对应的 RST，再改代码
2. L3 未定义符号须在 ``host_export_allowlist.json`` 内，声明来自
   ``minihub_plugin_sdk.h`` — :doc:`plugins/develop_l3`
3. 在设备上验证：串口日志、``GET /api/modules`` 、必要时 ``POST /api/hub/reload``

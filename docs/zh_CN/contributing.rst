贡献指南
========

:link_to_translation:`en:[English]`

感谢改进 MiniHub。本项目以 **开放、文档驱动** 为目标：二次开发者应依赖编程指南与
公开头文件，而不是阅读 Core 内部实现。

许可
----

贡献采用与仓库相同的 Apache License 2.0（根目录 LICENSE 文件）。

变更应放在哪里
--------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - 变更类型
     - 位置 / 原则
   * - OEM Host 产品（板级、分区、出厂配置）
     - ``application/APP/`` — 见 :doc:`application/index` 再 :doc:`host_customization`
   * - 厂商云 / 生态逻辑
     - ``examples/plugins/ID/`` 或 ``MINIHUB_PLUGIN_DIRS`` 的 Store
       ``.mhub`` — 请勿写进 Host Core
   * - 共享 Host Core 修复
     - ``components/`` 下 ``minihub_`` 前缀组件、``adapters/`` — 保持生态中立
   * - 设备类型目录（文案 / 控件）
     - ``components/minihub_device_widget/model/*.csv`` — 一台设备一条点；
       不要按类型加 C 结构。见 :doc:`type_catalog`
   * - 面向用户的文档
     - docs 下 en 与 zh_CN 目录须一致
   * - 模块 ABI / 导出符号
     - ``docs/module_sdk/``、:doc:`api/module_abi`

请勿在通用 Host 的 ``config/`` 出厂配置中写入厂商云 token、OEM 密钥或厂商专用默认规则。

Git 工作流
----------

* **提交信息：** 约定式提交（``feat:``、``fix:``、``docs:`` 等）
* **分支名：** 使用下划线，不用连字符 — 例如 ``feature/modbus_tcp_client``
  （见仓库 ``AGENTS.md`` 与 :doc:`vibe_coding` ）
* **作者：** 使用人类 ``user.name`` / ``user.email``；提交信息中不要加 AI 共著 trailer

文档贡献
--------

1. 同时修改 ``docs/en/`` 与 ``docs/zh_CN/`` （CI 会对比两棵 ``.rst`` 树）
2. 本地构建（输出 ``docs/_build/``）：

   .. code-block:: bash

      ./tools/build_docs.sh

   或 ``cd docs`` 后执行 ``build-docs -bs html -t esp32s31 -l en`` （及 ``zh_CN``）。
   勿在仓库根目录使用 ``build-docs --source-dir docs``。
   GitLab 任务 ``docs:html`` （``pre_check``）会编英/中文，Sphinx warning 即失败。

3. Sphinx warning 会导致构建失败 — 反引号字面量中避免尖括号占位符；请用 ``APP`` 或 ``ID`` 代替
4. 中文标题下划线宽度须不少于标题显示宽度（按东亚字符宽度计）

目录说明见 ``docs/README.md``。内部设计笔记在 ``docs/superpowers/``，不属于公开 toctree。

代码与固件
----------

* 构建参考 Host：``./tools/build_firmware.py -app minihub build``
* 修改 ``manifest.yml`` 后直接编译（``build_firmware.py`` 会自动合并）
* L3 模块：``./tools/build_firmware.py -app minihub modules`` （编译全部原生 ``.so``；打包用 ``pack``）
* L3 演示单独编译（不编 IDF 工程）：``python3 tools/test_build_plugin_standalone.py -v``
  （GitLab 任务 ``build:plugin_standalone`` ；需要 ``riscv32-esp-elf-gcc`` 与 ``cmake``）
* CI：根目录 ``.gitlab-ci.yml``

若变更 L3 模块可见的 Host 符号，需更新
``docs/module_sdk/host_export_allowlist.json`` 并运行
``python3 tools/gen_host_export_symbols.py`` （见 :doc:`plugins/develop_l3` ）。

评审要点
--------

* Host Core 变更不得引入厂商专用默认项
* 插件变更若新增 ``module.json`` 字段或 REST RPC，应在编程指南或 :doc:`api/plugin_rest` 中说明
* ABI 升级须同步 ``abi_version``、``core_api_min`` 与 ``core_api_max`` 文档及 :doc:`production` 中的打包说明

问题反馈
--------

Bug 与功能请求请使用项目 Issue 跟踪。架构类问题请先读 :doc:`introduction` 与
:doc:`architecture` — 若指南不清晰，补文档往往是合适的贡献方式。

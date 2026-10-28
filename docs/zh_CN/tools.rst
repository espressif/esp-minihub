PC 端脚本
=========

:link_to_translation:`en:[English]`

``tools/`` 下每个能跑的脚本都在下面的表里：它做什么、怎么调用、本指南哪一页解释过
它（写着「仅本页」就是别处没有）。常用流程的手写讲解在仓库的 ``tools/README.md`` ，
本页是索引。

多数脚本共用两个环境变量：

* ``MINIHUB_APP``—— 你正在构建或发布的是 ``application/`` 下的哪个产品。缺省
  ``minihub``，与 ``-app`` 同一个值。
* ``MINIHUB_PLUGINS_DEPLOY_DIR``—— 打包、发布、提供服务的脚本写入的部署根目录。缺省
  ``~/workspace/minihub_store``。一棵树，一个单元一个目录，目录结构见
  :doc:`api/deploy_index`。

构建一个产品
------------

.. minihub-tools:: Build a product

打包与发布
----------

.. minihub-tools:: Pack and publish

检查与门槛
----------

这些会作为构建或文档构建的一部分跑；它们报错是真缺陷，不是风格偏好。
``tools/ci/install_git_hooks.sh`` 负责装上那个把工具署名从提交历史里剥掉的 commit-msg
钩子，钩子本体在 ``tools/git-hooks/``。

.. minihub-tools:: Check and gate

与运行中的 Hub 打交道
---------------------

.. minihub-tools:: Work with a running Hub

给插件作者
----------

``minihub_plugin_mcp.py`` 是给编辑器（Cursor、VS Code）用的 stdio MCP 服务器：在 IDE 里
搭骨架、校验、打包插件。它 **不是** Hub 自己那个给智能体连的端上 MCP 服务器
（ :doc:`api/mcp`），两者除了协议没有共同点。实现放在 ``tools/plugin_mcp/``。

.. minihub-tools:: For plugin authors

指南本身
--------

.. minihub-tools:: The guide itself

``docs/`` 由 ``build_docs.sh`` 一次构建两种语言，警告即错误，并比较 en/zh 的文件树。

相关
----

* :doc:`getting_started`——第一次构建，一步一步
* :doc:`host_customization`、 :doc:`kconfig`——做一个产品，以及它的选项
* :doc:`plugins/overview`——动手搭插件之前先看清插件是什么
* :doc:`api/deploy_index`——这些脚本填的那棵部署树
* :doc:`production`——烧录工装需要从 ``Firmwares/`` 拿到什么

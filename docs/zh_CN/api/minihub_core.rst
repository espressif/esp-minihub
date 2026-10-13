MiniHub Core API
================

:link_to_translation:`en:[English]`

概述
----

MiniHub Core 提供 **点位注册表** 、 **状态存储** 、 **事件总线** 与 **命令路由** 。
Core **不按设备类型定义结构**：一台物理设备一条 ``minihub_point_t``
（``point_id`` = ``协议.设备``）。实时参数在 ``props.params``；
``minihub_point_set`` 发布 ``point.updated``。
``minihub_point_resolve_by_type_param`` 把目录 ``(类型, 参数)`` 映到该类型
最先登记的设备。

Host 内静态适配器直接调用本页 API。
适配器注册与 ``poll`` / ``handle_command`` 见 :doc:`minihub_adapter`。

.. only:: not hide_l3

   Store **L3** 模块须改用 :doc:`module_abi` 虚表。

.. include-build-file:: inc/minihub_core.inc

配置持久化（相关）
------------------

``minihub_config_save_adapters`` 写入 ``/data/config/adapters.json``
（``components/minihub_config/include/minihub_config.h``）。数据根路径：
``minihub_paths_data_root()`` 返回 ``/data``。

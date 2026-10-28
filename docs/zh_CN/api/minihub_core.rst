MiniHub Core API
================

:link_to_translation:`en:[English]`

概述
----

MiniHub Core 提供 **点位注册表**、 **状态存储**、 **事件总线** 与 **命令路由**。
Core **不按设备类型定义结构**：一台物理设备一条 ``minihub_point_t``
（``point_id`` = ``协议.设备``）。实时参数在 ``props.params``；只有 value 或
``props`` 真的变了，``minihub_point_set`` 才发布 ``point.updated``，防抖窗口内的
重复写入会被合成一次收尾发布。``minihub_point_resolve_by_type_param`` 把目录
``(类型, 参数)`` 映到该类型中 **``props.params`` 带这个参数** 的第一条点位；
只有类型、没有该参数的点位不匹配。旧式的按参数拆点 id
（``<协议>.<设备>.<参数>`` ）仍按最后一段匹配。

Host 内静态适配器直接调用本页 API。
适配器注册与 ``poll`` / ``handle_command`` 见 :doc:`minihub_adapter`。

.. only:: not hide_l3

   商店 **L3** 模块须改用 :doc:`module_abi` 虚表。

.. include-build-file:: inc/minihub_core.inc

配置持久化（相关）
------------------

``minihub_config_save_adapters`` 写入 ``/data/config/adapters.json``
（``components/minihub_config/include/minihub_config.h``）。数据根路径：
``minihub_paths_data_root()`` 返回 ``/data``。

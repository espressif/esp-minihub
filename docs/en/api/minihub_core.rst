MiniHub Core API
================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

MiniHub Core maintains the **point registry**, **state store**, **event bus**, and
**command router**. Core is type-agnostic: one physical device is one
``minihub_point_t`` (``point_id`` = ``protocol.device``). Live parameters live
in ``props.params``; ``minihub_point_set`` publishes ``point.updated``.
``minihub_point_resolve_by_type_param`` maps catalog ``(type, parameter)`` to
the first registered device of that type.

Static Host adapters call these functions directly. Adapter registration and
``poll`` / ``handle_command`` are documented in :doc:`minihub_adapter`.

.. only:: not hide_l3

   Store **L3** modules must use the vtable in :doc:`module_abi` instead.

.. include-build-file:: inc/minihub_core.inc

Configuration persistence (related)
-----------------------------------

``minihub_config_save_adapters`` writes ``/data/config/adapters.json``
(``components/minihub_config/include/minihub_config.h``). Data root:
``minihub_paths_data_root()`` returns ``/data``.

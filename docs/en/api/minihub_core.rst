MiniHub Core API
================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

MiniHub Core maintains the **point registry**, **state store**, **event bus**, and
**command router**. Core is type-agnostic: one physical device is one
``minihub_point_t`` (``point_id`` = ``protocol.device``). Live parameters live
in ``props.params``; ``minihub_point_set`` publishes ``point.updated`` when the
value or ``props`` actually changed, and repeats inside the debounce window are
folded into one trailing publish. ``minihub_point_resolve_by_type_param`` maps a
catalog ``(type, parameter)`` pair to the first registered point of that type
whose ``props.params`` carries that parameter — a point that has the type but not
the parameter does not match. Legacy per-parameter ids
(``<protocol>.<device>.<parameter>``) still match on that last segment.

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

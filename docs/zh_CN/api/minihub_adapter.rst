MiniHub Adapter API
===================

:link_to_translation:`en:[English]`

概述
----

静态或模块导出的适配器实现 ``minihub_adapter_t``。Host 对 enabled 适配器的典型顺序：

1. ``init(config)`` — ``adapters.json`` 中该适配器子树
2. ``start()``
3. 周期 ``poll()`` （可选）
4. ``handle_command(cmd)`` — 同 REST ``POST /api/command``。目录设备可在
   ``cmd.params`` 里带 ``parameter``，只更新 **这一台设备点** 上的
   ``props.params`` （不要按参数再拆点）。
5. 重载或关机：``stop()`` · ``deinit()``

静态适配器用 ``REGISTER_ADAPTER(symbol)``；``adapters_setup.c`` 注册清单内适配器。
卸载动态模块前应 ``minihub_adapter_lifecycle_lock()``，避免 ``poll`` 在即将卸载的
``.mhub`` 内运行。

点位/命令/事件类型见 :doc:`minihub_core`。

.. include-build-file:: inc/minihub_adapter.inc

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

6. 可选 ``resync(config)`` — 只有配置变化（新增一台设备、或某台设备的状态推进）时，
   **原地** 重发本适配器的点位，不走 ``stop()`` / ``start()``。Host 侧经
   ``minihub_adapter_resync()`` 调用；URI handler 和 RPC 应该用
   ``minihub_hub_resync_adapter_async()``，因为 resync 要等适配器生命周期锁，而
   ``poll`` 可能持锁数秒。留 ``NULL`` （或返回 ``ESP_ERR_NOT_SUPPORTED``）时，worker
   回落到整次 reload —— 那会注销所有适配器的点位，直到 ``start_all()`` 才恢复，控制台
   就会看到设备先消失再出现。Matter 适配器实现了它，配网卡片因此能原地切换状态。

静态适配器用 ``REGISTER_ADAPTER(symbol)``；``adapters_setup.c`` 注册清单内适配器。
卸载动态模块前应 ``minihub_adapter_lifecycle_lock()``，避免 ``poll`` 在即将卸载的
``.mhub`` 内运行。

点位/命令/事件类型见 :doc:`minihub_core`。

.. include-build-file:: inc/minihub_adapter.inc

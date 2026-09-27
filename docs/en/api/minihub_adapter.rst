MiniHub Adapter API
===================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

Static and module-exported adapters implement ``minihub_adapter_t``. Typical Host
call order for an enabled adapter:

1. ``init(config)`` — adapter subtree in ``adapters.json``
2. ``start()``
3. Periodic ``poll()`` (optional)
4. ``handle_command(cmd)`` — same path as REST ``POST /api/command``. For
   catalog devices, ``cmd.params`` may include ``parameter`` so you update
   ``props.params`` on **that one device point** (do not register one point
   per parameter).

5. On reload or shutdown: ``stop()`` · ``deinit()``

6. Optional ``resync(config)`` — republish this adapter's points **in place** after a
   config-only change (a device row appeared, or its status moved) without ``stop()`` /
   ``start()``. Host reaches it through ``minihub_adapter_resync()``; handlers and RPC
   should use ``minihub_hub_resync_adapter_async()``, because a resync waits on the
   adapter lifecycle lock that ``poll`` can hold for seconds. Leave it ``NULL`` (or
   return ``ESP_ERR_NOT_SUPPORTED``) and the worker falls back to a full reload — which
   unregisters every adapter's points until ``start_all()`` finishes, so the console
   visibly loses and re-adds the devices. The Matter adapter implements it to keep
   commissioning cards on screen while they change state.

Register static adapters with ``REGISTER_ADAPTER(symbol)``; generated
``adapters_setup.c`` registers manifest adapters at boot. Before unloading a dynamic
module, hold ``minihub_adapter_lifecycle_lock()`` so ``poll`` cannot run inside a
``.mhub`` about to be unloaded.

Point / command / event types live in :doc:`minihub_core`.

.. include-build-file:: inc/minihub_adapter.inc

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

Register static adapters with ``REGISTER_ADAPTER(symbol)``; generated
``adapters_setup.c`` registers manifest adapters at boot. Before unloading a dynamic
module, hold ``minihub_adapter_lifecycle_lock()`` so ``poll`` cannot run inside a
``.mhub`` about to be unloaded.

Point / command / event types live in :doc:`minihub_core`.

.. include-build-file:: inc/minihub_adapter.inc

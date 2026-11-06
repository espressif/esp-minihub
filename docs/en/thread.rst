Thread
======

:link_to_translation:`zh_CN:[中文]`

MiniHub's ``thread`` adapter is an **OpenThread Border Router** (operational
dataset / 802.15.4 radio **mutually exclusive** with Zigbee). Thread takes the
2.4 GHz radio to itself — neither Wi-Fi nor BLE Mesh runs beside it, so a Thread
Hub reaches the LAN over Ethernet. It is **not** a device adapter. Summary table:
:doc:`radios`.

A border router routes rather than owns devices, so this adapter registers no
Hub points and has no ``devices[]``; ``handle_command`` returns not supported.
Matter-over-Thread end-devices show up under ``adapters.matter``
(``transport=thread``), which is also where manufacturer, associate and OTA live —
see :doc:`matter`.

Enable ``CONFIG_MINIHUB_THREAD_ADAPTER`` (exclusive with Zigbee). That option also
``select``s ``MINIHUB_MATTER_ADAPTER``, so a Thread build always carries the Matter
commissioner. Dataset: ``GET`` / ``POST /api/thread/dataset``, and the live role,
channel and ``dataset_set`` flag come from ``GET /api/thread/status`` — the
``adapters.thread.channel`` key in the factory JSON is not read by anything.
Fields: :doc:`data_model`
(:ref:`adapters-JSON`).

Start or stop the border router (no body; same login as other writes). These
routes exist only with that option:

* ``POST /api/thread/start`` — JSON ``status`` is ``ok``, or an ``esp_err``
  name plus optional ``message``
* ``POST /api/thread/stop`` — same ``status`` field; ``ok`` when the stack was
  already down

A JSON pack that injects a Thread ``devices[]`` creates paper devices nothing can
talk to, and a ``thread_ota`` plugin would have nothing to update, so do not
ship one. That is a convention, not an enforced limit: the validator lists
``thread`` as an injectable Host adapter and the Host pack loader merges the
devices into ``adapters.thread``, where the border router ignores them. Such a
pack loads normally and simply has nothing to talk to.

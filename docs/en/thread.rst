Thread
======

:link_to_translation:`zh_CN:[中文]`

MiniHub's ``thread`` adapter is an **OpenThread Border Router** (dataset /
channel / 802.15.4 radio **mutually exclusive** with Zigbee). It is **not** a
device adapter. Summary table: :doc:`radios`.

It does **not** register Hub points and has **no** ``devices[]``.
``handle_command`` returns not supported. Matter-over-Thread end-devices show
up under ``adapters.matter`` (``transport=thread``); manufacturer, claim, and
OTA all live on **Matter** points — see :doc:`matter`.

Enable ``CONFIG_MINIHUB_THREAD_ADAPTER`` (exclusive with Zigbee). Dataset:
``GET`` / ``POST /api/thread/dataset``. Fields: :doc:`data_model`
(:ref:`adapters-json`).

Do not L1-inject a Thread device list, and do not ship a ``thread_ota`` plugin.

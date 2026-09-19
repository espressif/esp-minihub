Network C API
=============

:link_to_translation:`zh_CN:[中文]`

Overview
--------

``minihub_net`` wraps Host uplink (Ethernet / Wi-Fi STA) initialization, IPv4 queries,
and DNS diagnostics. Boot order and Kconfig: :doc:`../networking`.

Modules should use ``minihub_net_*``, not ``esp_wifi_*`` directly (not on the L3
allowlist). Persistent interface config is in ``minihub_net_cfg.h``; REST entry:
``POST /api/net/config`` in :doc:`host_rest`.

API Reference
-------------

.. include-build-file:: inc/minihub_net.inc

.. include-build-file:: inc/minihub_net_cfg.inc

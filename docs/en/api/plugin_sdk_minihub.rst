Plugin SDK — MiniHub
====================

:link_to_translation:`zh_CN:[中文]`

Overview
--------

MiniHub Host exports used by Store ELF plugins: **points**, **module ABI**,
**config / hub reload**, **HTTP worker**, **net probe**, and **BLE GATT client**.

Out-of-tree plugins get the same declarations from
``minihub_plugin_sdk/minihub.h`` (via ``#include "minihub_plugin_sdk.h"``).

Prefer the Host vtable in :doc:`module_abi` for Core services. Point types and
registry APIs: :doc:`minihub_core`. Adapter vtable: :doc:`minihub_adapter`.
Network helpers: :doc:`net_api`.

HTTP worker
-----------

.. include-build-file:: inc/minihub_http_worker.inc

BLE GATT client
---------------

GATT **client**, not BLE Mesh radio.

.. include-build-file:: inc/minihub_ble_gatt.inc

Related
-------

* Module entry / ``minihub_module_host_api_t``: :doc:`module_abi`
* Points / commands / events: :doc:`minihub_core`
* ``minihub_net_has_ip`` / ``minihub_net_dns_probe``: :doc:`net_api`
* Allowlist + ABI check: :doc:`plugin_host`

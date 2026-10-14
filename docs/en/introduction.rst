Introduction
============

:link_to_translation:`zh_CN:[中文]`

MiniHub is Host firmware for a multi-protocol IoT hub on Espressif ESP32-S31
class SoCs: Web UI, REST, Store dynamic modules (mhub packages), and adapters for
Zigbee / BLE Mesh / Matter / MQTT / Modbus, and more.

The generic Host is ecosystem-agnostic. Espressif-owned stacks and hub services
stay in Core. Vendor clouds and OEM differences can ship in two ways: as Store
plugins (factory preinstall per SKU, or field install/upgrade when the ABI
matches) without maintaining Host code; or as a Host-based product fork, which
uses less RAM and can call Plugin SDK and ESP-IDF APIs more freely. Automation
uses points: **one device = one point** (``point_id`` like ``sim.js_kitchen``),
with live parameters in ``props.params``, not per-vendor APIs. Type labels and
widgets come from the Host catalog, not from Core C.

In the repository, ``application/APP/`` is a product (board, partitions, factory
JSON); ``components/`` holds shared Core and adapters. Product tree:
:doc:`application/index`. This guide documents
manifests, JSON, REST/MCP, and the module ABI for secondary development — without
reading Core C. Released under Apache 2.0; see :doc:`contributing`.

First build and flash: :doc:`getting_started`. Layering, boot order, storage,
and soft reload: :doc:`architecture`. Points and Hub config:
:doc:`data_model`. Device types and UI: :doc:`type_catalog`. How radios
register points, claim, and OTA: :doc:`radios`.

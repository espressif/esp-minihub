Introduction
============

:link_to_translation:`zh_CN:[中文]`

What the firmware does
----------------------

A MiniHub build is one firmware image, and the jobs below all run on the device.

Bundled adapters cover Wi-Fi STA, Zigbee, BLE Mesh, Matter, Thread as a border
router, MQTT and Modbus. Rules are
stored on the device and evaluated there
(:doc:`rules`), and the web console, the REST API and the WebSocket stream are
served by the device as well, so LAN control survives an internet outage.

Protocol or cloud behavior that is not in your firmware can still arrive later,
because a Store module is installed at run time and needs no rebuild
(:doc:`plugins/overview`). The console and your own integration use the same
REST, WebSocket and JavaScript surface (:doc:`api/index`), which is also what
the optional MCP server exposes: with ``CONFIG_MINIHUB_MCP_SERVER`` an assistant
calls the same operations a browser does (:doc:`api/mcp`).

Who reads this guide
--------------------

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Reader
     - What you are deciding
     - Chapters that matter
   * - Evaluating the platform
     - Protocol coverage, offline behavior, license, limitations
     - This page, :doc:`architecture`, :doc:`radios`, :doc:`faq`
   * - Product engineer
     - Board, partitions, feature flags, factory config, field update
     - :doc:`getting_started`, :doc:`application/index`,
       :doc:`host_customization`, :doc:`production`
   * - Plugin author
     - One developer cloud or device family, without a Core change
     - :doc:`plugins/first_plugin`, :doc:`plugins/overview`,
       :doc:`api/js_host`
   * - Integrator
     - Your own app, cloud, or console talking to a Hub
     - :doc:`api/host_rest`, :doc:`data_model`, :doc:`rules`, :doc:`data_flow`

None of these paths asks you to read the Core C sources first. The contracts
are JSON files, HTTP endpoints, and one module ABI.

Why the code is split this way
------------------------------

The split comes from one requirement. A hub has to work with ecosystems that
did not exist when its firmware was compiled, and none of those ecosystems may
become a build dependency of the firmware.

.. list-table::
   :header-rows: 1
   :widths: 16 32 52

   * - Layer
     - What it owns
     - Why the boundary is there
   * - Core
     - Point registry, command routing, event bus, config files
     - Core holds a device as a type *name* plus a JSON block of parameters,
       so a new kind of device needs no Core change (:doc:`data_model`)
   * - Adapters
     - Protocol traffic in, point updates out
     - A rule refers to a point, so swapping the radio under a device does not
       rewrite the automation (:doc:`radios`)
   * - Type catalog
     - Labels, widgets, icons, ranges per type name
     - Display is table data, so a device type is a CSV row rather than a C
       struct (:doc:`type_catalog`)
   * - Store modules
     - Vendor clouds, extra device families, module-owned UI
     - Installed and upgraded in the field; they carry the credentials that
       must not be in a generic build (:doc:`plugins/index`)
   * - Product
     - Board, partition table, sdkconfig, factory JSON
     - SKU differences stay under ``application/``, so products share one Core
       (:doc:`application/index`)

A stack that works without a vendor account or server may link statically into a
Host. Anything that needs one arrives as a Store module or as your own product
tree.

Limitations
------------

* Nothing checks a vendor device type while you compile, because Core stores a
  type as a name. A wrong ``type`` or parameter shows up at run time instead: the
  card renders with no control and the console logs a warning
  (:doc:`type_catalog`).
* All L2 modules share one QuickJS task. A callback that blocks therefore holds
  up every other JS module as well (:doc:`api/js_host`).
* Wi-Fi and BLE Mesh coexist on the 2.4 GHz radio; IEEE 802.15.4 (Zigbee **or**
  Thread) coexists with neither, and Zigbee and Thread are never started together
  (:doc:`radios`).
* The console is plain HTTP on the LAN; the Host web stack has no TLS listener.
  Log in with a session or send an optional API token, and put TLS in front of
  the Hub when the deployment needs it (:doc:`api/host_rest`).
* The generic layout assumes two writable partitions, ``storage`` for Hub config
  **and** the Web console, and ``modstore`` for installed modules. A smaller flash part
  may not fit it (:doc:`production`).

Where to go next
----------------

Build and flash: :doc:`getting_started`. Layers, boot order, storage, and soft
reload: :doc:`architecture`. One value from device to browser and back:
:doc:`data_flow`. Unknown word: :doc:`glossary`. Unknown directory:
:doc:`source_tree`.

The project is under the Apache License 2.0; contribution notes are in
:doc:`contributing`.

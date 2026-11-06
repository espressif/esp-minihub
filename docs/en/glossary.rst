Glossary
========

:link_to_translation:`zh_CN:[中文]`

The device model
----------------

Host
    The firmware image you flash, and the device running it. Used to separate
    the device side from a PC or a cloud service.

Firmware and image
    Two words for one thing, used apart on purpose. **Firmware** is the software a
    device runs; an **image** is the artifact that gets flashed, verified or sized —
    the app binary sitting in an OTA slot, a LittleFS image, a merged flash image.
    So a slot holds an image and a device runs firmware: "the update failed" is
    about the image, "it will not boot" is about the firmware.

HUB
    Placeholder for the Hub's LAN address in URLs such as ``http://HUB/``. It is
    the address the device prints as ``eth got IP`` or ``wifi got IP``, not a
    hostname the firmware registers.

Core
    The shared, protocol-neutral code under ``components/minihub_core`` and its
    neighbours. Core knows the *shape* of a device, not its *kind*. The *shape*
    is the device's structure or skeleton; the *kind* is its concrete category
    (a light, a switch, and so on). Core is a protocol- and type-agnostic engine:
    it treats every device uniformly as a *point*, so adding a new device category
    only needs an adapter that writes points and one catalog row — Core itself
    stays untouched.

Product
    One folder under ``application/``, named by its folder (``minihub`` is the
    generic Host). A product carries board, partition table, sdkconfig, and
    factory JSON. See :doc:`application/index`.

SKU (stock keeping unit)
    A concrete, shippable variant of a Product: one ``application/<sku>/`` folder
    that keeps the board, partition table, sdkconfig, and factory JSON differences
    while sharing the same Core. One Core serves every SKU; see
    :doc:`application/index`.

.. _point:

Point
    One device, as Core sees it: a ``minihub_point_t`` with an id, a type name,
    a summary value, a JSON block of parameters, the adapter that owns it, and
    an online flag. One physical device is exactly one point; do not register more than one point for the same device. See :doc:`data_model`.

``point_id``
    The point's address, spelled ``<protocol>.<device>``: ``zigbee.zb_7d4b``,
    ``thread.matter_n2_ep1``, ``sim.js_kitchen``. The prefix names the transport
    that registered the point, not the device type — for a radio adapter it is
    the adapter's own name (``zigbee.``, ``ble_mesh.``), and for Matter it is the
    **commissioning** transport (``wifi.`` / ``ethernet.`` / ``thread.``).

Type
    The category of a device, for example ``light`` or ``switch``, which determines what icon is used to render the device.

Parameter
    One live setting of a device, held at ``props.params.<name>`` with an
    optional ``access``, ``visible``, ``min``, ``max``, ``step``, ``unit``.
    Example: ``power``, ``brightness``, ``cct``.

Type catalog (device widget)
    The table that says how a type and each of its parameters are rendered:
    label, widget, icon, bounds. Its source of truth is
    ``components/minihub_device_widget/model/*.csv``, and the device reads a
    flattened copy from ``/data/config/devices.csv`` at boot. See
    :doc:`type_catalog`.

Widget
    A control kind the catalog can name — a closed set of six: ``toggle``,
    ``slider``, ``dropdown``, ``text``, ``button``, ``hidden``.

Adapter
    Code that speaks one protocol and writes the results into points. Seven
    ship with the generic Host: wifi, zigbee, ble_mesh, matter, thread, mqtt,
    modbus (:doc:`radios`). Each is switched on by a key under ``adapters`` in
    ``adapters.json``.

Associate
    Linking a registered point to a plugin adapter's name (writing
    ``props.owner``). A generic device adapts successfully even without the
    user's adapter; but features that need differentiated handling, such as
    OTA, must be associated with the right adapter to work. Associating never
    creates a second point.

Rule
    One ``when`` / ``if`` / ``then`` automation in ``/data/config/rules.json``,
    evaluated on the device. See :doc:`rules`.

Scene
    A named set of commands invoked as a unit through
    ``POST /api/scenes/invoke``. A scene is not a rule action type.

Event bus
    The in-firmware pub/sub that every **changed** point update passes through.
    The WebSocket at ``/ws/events`` mirrors it, and ``point.updated`` is the
    event the console and your integration subscribe to.

The packaging model
-------------------

Store
    Two things: the on-device subsystem ``minihub_store`` (refresh the catalog,
    install, upgrade, uninstall) and the server that feeds it. See
    :doc:`plugins/pack_and_catalog`.

QuickJS
    The embedded JavaScript engine that runs JS modules. This firmware uses the
    ``espressif/quickjs-ng`` component (QuickJS-NG 0.14.0), not upstream QuickJS
    and not a browser: no module system, no ``console``, no ``fetch``. It is
    single-threaded and every JS module shares one worker task. The consequences
    are listed in :doc:`plugins/develop_js`.

.. only:: not hide_l3

   A third tier, ELF, carries a native ``.so`` instead of data or JavaScript.

   MHMD
       The magic number ``0x4D484D44`` of the inner native image in an ELF
       package. It names the binary, not the zip around it.

   ABI
       The C contract between a native module and the Host: one vtable of Host
       functions plus a hash of the allowed symbols. ``abi_version`` is 1 and
       ``MINIHUB_CORE_API_VERSION`` is 1. Native modules may link only against
       the symbols listed in ``docs/module_sdk/host_export_allowlist.json``;
       the contract is on :doc:`api/module_abi`.

   ``core_api_min`` / ``core_api_max``
       The Host Core API range a package declares. A module loads only when the
       running firmware's version falls inside the range.

Deploy root
    The PC-side folder tree a Hub pulls packages from, default
    ``~/workspace/minihub_store``, holding ``Stores/``, ``Firmwares/``,
    ``Assets/``, and ``Webs/``. Served by ``tools/store_server.py``.

``catalog.json``
    The store entry for one plugin, next to its ``.mhub``. The PC server
    (``tools/store_server.py``) aggregates these files at request time for
    ``GET /api/catalog``; no combined index is written to disk. The Hub asks that
    endpoint and republishes what it holds on its own ``GET /api/store/catalog``
    (:doc:`api/plugin_rest`).

Runtime and storage
-------------------

``storage`` partition
    LittleFS volume mounted at ``/data``: Hub configuration, rules, scenes, the
    flattened type catalog, and the console.

``modstore`` partition
    LittleFS partition mounted at ``/store``: installed Store modules under
    ``/store/modules/`` and, on some products, the preinstalled console copy.

NVS
    The ESP-IDF key-value store.

Soft reload (hub reload)
    Restarting adapters and modules in place, without a reboot:
    ``POST /api/hub/reload`` or MCP ``hub.reload``. It re-reads JSON, stops
    adapters, reloads modules, and re-runs each JS ``setup(host)``. Matter and
    Thread builds may re-attach CHIP to OpenThread.

Names that mean two things here
-------------------------------

Console (web console)
    The browser UI served by the device from ``/data/web``. Skinnable and
    replaceable (:doc:`web_console`). Do not confuse it with a serial terminal.

MCP
    Two unrelated things use the name. The **on-device** MCP server
    (``CONFIG_MINIHUB_MCP_SERVER``, needs ``components/esp-claw``) lets an AI
    assistant drive a live Hub. The **plugin-author IDE MCP**
    (``tools/minihub_plugin_mcp.py``) runs on your PC and only scaffolds,
    validates, and packs modules.

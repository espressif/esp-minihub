Repository Layout
=================

:link_to_translation:`zh_CN:[中文]`

.. contents::
   :local:
   :depth: 2

Top level
---------

.. code-block:: text

   application/       One folder per product; this is what you flash
   components/        Core, services, protocol adapters
   configs/           Shared factory JSON and build-time defaults
   cmake/             Build glue: product bundle, module .so rules, assets
   tools/             PC-side scripts (build, pack, serve, docs)
   examples/plugins/  In-tree Store module sources, for reference
   docs/              This guide, plus the plugin SDK surface
   README.md

.. list-table::
   :header-rows: 1
   :widths: 20 44 36

   * - Path
     - What it holds
     - Open it when
   * - ``application/``
     - Products: board, partition table, sdkconfig recipe, factory JSON,
       ``app_main``. The folder name is the product name.
     - You are shipping your own SKU
       (:doc:`application/index`, :doc:`host_customization`)
   * - ``components/``
     - Shared Core, services, and adapters. Every product links these; do not
       copy them into a product.
     - You are changing Host behavior
   * - ``configs/``
     - ``minihub_manifest.yml`` (the ``_host_base`` manifest all products
       extend), ``adapters.json``, ``rules.json``, ``scenes.json``,
       ``sdkconfig/``, ``product_params/``.
     - A default should change for every product
   * - ``cmake/``
     - ``minihub_bundle/`` (the include and Kconfig glue a product pulls in),
       ``minihub_module_so.cmake`` and ``minihub_plugin_standalone.cmake``
       (native module builds), ``minihub_product_assets.cmake``,
       ``matter_no_rtti.cmake``, ``check_shared_object.cmake``.
     - Usually not; a product profile covers most needs
   * - ``tools/``
     - Scripts that run on your PC, not on the Hub.
     - Packing, serving, generating
   * - ``examples/plugins/``
     - Demo Store modules (``color_bulb_js``, ``meter_plug_js``,
       ``ble_mesh_pack``). Your own plugin tree can sit beside the repo through
       ``MINIHUB_PLUGIN_DIRS``.
     - You want a starting point to copy
   * - ``docs/``
     - ``en/`` and ``zh_CN/`` (this guide, mirrored file for file),
       ``module_sdk/`` (the plugin-facing header set, templates, and export
       allowlist), ``_extensions/`` and ``_static/`` (build helpers).
     - Something in the guide is wrong or missing

``components/``, by job
-----------------------

.. image:: ../_static/source_tree/component_map.svg
   :align: center
   :alt: MiniHub components grouped into five lanes by job: core and services, device model and UI, store and modules, radios, optional hardware

Core and services
    These carry the Hub itself: the point registry, the files it reads, and the
    link it talks over.

    The point registry, command router, event bus and worker tasks are in
    ``minihub_core`` ; reading and writing ``/data/config/*.json`` is
    ``minihub_config`` ; the soft reload sequence is ``minihub_hub`` ; the console
    session and the optional API token are ``minihub_auth`` ; network credentials
    over SoftAP and the factory reset are ``minihub_provision`` ; Ethernet and
    Wi-Fi STA come up in ``minihub_net`` ; firmware update state and progress are
    ``minihub_ota`` ; staging firmware on external media and the Vibe Mode
    hand-off are ``minihub_vibe`` (Host library plus a nested ``subproject/``
    that emits the retained ``app,factory`` image — not a product under
    ``application/``) ; fetching and applying
    firmware, asset and console packages from the deploy root is
    ``minihub_update`` , and the on-device MQTT broker that the site's devices
    connect to is ``minihub_mqtt_broker`` .

Device model and UI
    This group decides how a device is presented in the UI and when automation fires.

    The type catalog — CSV in, labels and widgets out, icons with it — is
    ``minihub_device_widget`` ; ``when`` / ``if`` / ``then`` evaluation is
    ``minihub_rules`` , and one call that recalls a scene is ``minihub_scenes`` ;
    the HTTP routes, the event bus mirrored on ``/ws/events`` and the delivery of
    the console and map tiles are ``minihub_web`` ; the on-device MCP tools (they
    need ``components/esp-claw`` ) are ``minihub_mcp_server`` ; the product-level
    overrides applied onto Zigbee and BLE Mesh props are
    ``minihub_product_params`` .

Store and modules
    The Store exists so plugins can be installed at run time, which is what makes
    the extension flexible. Catalog refresh, install, upgrade and uninstall are
    ``minihub_store`` ; reading manifests, checking the ABI and loading are
    ``minihub_module`` ; merging JSON packs into the adapters view is
    ``minihub_pack`` ; the QuickJS worker and the ``host`` object are
    ``minihub_js`` .

Radios
    The stack glue for Zigbee, BLE Mesh, Matter and Thread is one folder each —
    ``minihub_zigbee`` , ``minihub_ble_mesh`` , ``minihub_matter`` ,
    ``minihub_thread`` — and the generic NimBLE GATT client that modules use is
    ``minihub_ble_gatt`` .

    Radio/device adapters live under ``components/minihub_protocol_adapters/`` :
    ``adapter_zigbee`` , ``adapter_ble_mesh`` , ``adapter_matter`` ,
    ``adapter_thread`` , ``adapter_modbus`` . Four register device points —
    Zigbee, BLE Mesh, Matter and Modbus; Thread is an OpenThread Border Router
    and registers no device points (:doc:`radios`). Network uplink (Wi-Fi STA /
    SoftAP / Ethernet) is ``minihub_net`` (including the thin ``wifi`` adapter
    registration). The MQTT loopback client that carries ``host.mqtt`` lives in
    ``minihub_mqtt_broker`` next to the on-device broker. Each radio adapter is
    its own component, so a product can leave one out without touching the
    others.

Optional hardware
    These are compiled only when a board asks for them: the on-device UI for
    boards with a display is ``minihub_lcd`` (``CONFIG_MINIHUB_LCD_UI``, mutually
    exclusive with Ethernet on some boards); pulling and recording an RTSP stream
    is ``minihub_rtsp_cam`` , and the recording needs somewhere to land, so
    ``minihub_usb_msc`` mounts a USB flash drive at ``/usb0`` .

.. only:: not hide_l3

   ``minihub_module`` also loads native modules, and
   ``components/minihub_module/host_elf_symbols_generated.c`` is the Host symbol
   table they link against.

``tools/``, by what you are doing
---------------------------------

Build and flash
    ``build_firmware.py`` is the front door (``-app APP build``, ``app-flash``,
    ``monitor``, ``modules``). ``sync_host_manifest.py`` turns a product
    ``manifest.yml`` into the generated files the IDF build reads, and
    ``compose_sdkconfig.py`` does the same for sdkconfig fragments.
    ``gen_flash_binaries.py`` produces the factory flash set, and
    ``publish_firmware.py`` writes a release into ``Firmwares/``.

Store modules
    ``new_store_module.py`` scaffolds a tier from the templates.
    ``pack_store_module.py`` builds a ``.mhub``. ``sync_store_catalog.py``
    writes the matching ``catalog.json``. ``store_server.py`` serves a deploy
    root over HTTP for a Hub to pull from. ``build_store_modules.py`` compiles
    and packs what the Host build found.

.. only:: not hide_l3

   ``tools/build_plugin_standalone.py`` cross-compiles a native module outside
   the Host tree, and ``tools/check_plugin_sdk.py`` plus
   ``tools/check_module_so_layout.py`` verify that it links only against
   exported Host symbols.

Documentation
    ``build_docs.sh`` builds this guide (en, zh_CN, or all) and fails on a
    warning. ``preview_docs.py`` serves the built HTML.
    ``gen_deploy_index.py`` writes the asset manifests.

Module authors may rely on ``docs/module_sdk/`` alone: it holds the headers a
module may include, the symbol allowlist, and copies of the pack scripts for
out-of-tree builds. ``python3 tools/minihub_plugin_mcp.py`` hands the same
scaffolding and validation to an IDE.

Generated, do not hand-edit
---------------------------

The build writes these; your edits are overwritten on the next run.

.. list-table::
   :header-rows: 1
   :widths: 52 48

   * - Path
     - Written by
   * - ``application/APP/main/idf_component.yml``
     - ``tools/sync_host_manifest.py``
   * - ``application/APP/main/adapters_setup.generated.c``
     - ``tools/sync_host_manifest.py``
   * - ``cmake/generated/minihub_components.cmake``,
       ``cmake/generated/sdkconfig_stacks.gen``
     - ``tools/sync_host_manifest.py``, ``tools/compose_sdkconfig.py``
   * - ``components/minihub_device_widget`` embedded CSV and registry
     - ``components/minihub_device_widget/tools/gen_device_widget.py``
   * - ``components/minihub_module/include/minihub_abi_symbols.gen.h``
     - ``tools/gen_abi_contract.py``
   * - ``docs/_static/architecture/``
     - ``docs/_extensions/stage_architecture.py`` at docs build time
   * - ``/data/config/devices.csv`` on a device
     - seeded by firmware from the embedded catalog

Device paths
------------

What a running Hub writes is not in the repository. ``/data`` is the ``storage``
partition and ``/store`` is ``modstore``; the layout and which files are copied
from factory JSON on first boot are in :doc:`architecture`.

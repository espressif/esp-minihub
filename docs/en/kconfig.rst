Kconfig Options
===============

:link_to_translation:`zh_CN:[中文]`

Every MiniHub build option in one place, generated at docs build time from the same
Kconfig fragments as ``menuconfig``. Each entry notes which source file reads the
symbol, so enabling it can be checked against real call sites.

The authority is the ``rsource`` list in ``cmake/minihub_bundle/Kconfig.projbuild``: that
is exactly what the build mounts. A fragment that exists on disk but is not rsourced, a
``rsource`` line whose file was deleted, a symbol with no type or an ``int`` with no
default — any of these fails the docs build rather than publishing a shorter list.

Defaults here are the **Kconfig** defaults, i.e. what a build with no ``sdkconfig`` yet
gets. Your board's current values are whatever its ``sdkconfig`` says, which usually
differs.

How a build arrives at these values
-----------------------------------

A product's options are not written one by one. ``application/<app>/configs/sdkconfig/app.yml``
lists *features*, and each feature is a fragment that sets a group of symbols. The build
resolves the list, appends the local overrides, and hands the chain to ESP-IDF as
``SDKCONFIG_DEFAULTS``. Later entries win over earlier ones, which is why the generated
stack fragment is appended last.

.. minihub-features:: minihub

These rules in that chain decide what a change does:

* A feature name only groups symbols. When
  ``configs/sdkconfig/feature/<name>.defaults`` and
  ``application/<app>/configs/sdkconfig/feature/<name>.defaults`` both exist, both are
  loaded. The product file is later, so the same ``CONFIG_*`` symbol in it replaces the
  shared value. A symbol set only in the shared file stays.
* A ``sdkconfig`` that already exists wins over every fragment. Changing a
  ``feature/*.defaults`` file and rebuilding does nothing if the symbol is still set in
  ``application/<app>/sdkconfig`` — that is why ``tools/build_firmware.py`` rewrites the
  stack symbols (``CONFIG_MINIHUB_BLE_MESH_SDK`` and friends) in the product file itself
  instead of relying on the fragment, and why it deliberately does **not** reset symbols
  you did not select: that would undo a hand edit or a ``menuconfig`` session.
* The generated ``stacks.gen`` fragment comes after the product's own features, and
  ``local.overrides`` after that — so a lab-only value can sit in one file that never
  gets committed.
* ``board_manager.defaults`` is not in the list on purpose — ``esp_board_manager`` injects
  it. It is what pulls the board's own flash, PSRAM and PHY settings in.
* Thread and Zigbee may both be linked. Which one runs is the ``enabled`` flag
  of that entry in the product ``adapters.json``. When both are true, the one
  that starts later is skipped.

To see what a configured build actually uses, read the generated files rather than the
fragments: ``application/<app>/build/config/sdkconfig.h`` (what the C code sees),
``sdkconfig.cmake`` and ``sdkconfig.json`` next to it, or ``application/<app>/sdkconfig``
for the values that beat the defaults. ``tools/build_firmware.py`` prints the whole
resolved ``SDKCONFIG_DEFAULTS`` line before it invokes the build.

Core and memory
---------------

.. minihub-kconfig:: Core

Networking and provisioning
---------------------------

.. minihub-kconfig:: Networking

.. minihub-kconfig:: Provisioning

Plugins
-------

Store install path, native module loader, and QuickJS under ``Plugins`` in
``menuconfig``.

.. minihub-kconfig:: Store

.. minihub-kconfig:: Modules

.. minihub-kconfig:: JS plugins

Builtin Adapters
----------------

Host-linked protocol stacks under ``Builtin Adapters`` in ``menuconfig``
(Store ``.mhub`` packages are under Plugins). Includes the MQTT broker
(on-device listener; the loopback ``MINIHUB_MQTT_ADAPTER`` is invisible and
follows the broker) and RTSP camera (external IP cameras — not Hub board
peripherals). Wi-Fi / Ethernet uplink options live under Networking, not here.

.. minihub-kconfig:: BLE Mesh

.. minihub-kconfig:: Matter

.. minihub-kconfig:: MQTT broker

.. minihub-kconfig:: Modbus

.. minihub-kconfig:: RTSP camera

.. minihub-kconfig:: Thread

.. minihub-kconfig:: Zigbee

UI
--

LCD and Web consoles under ``UI`` in ``menuconfig``.

.. minihub-kconfig:: LCD UI

.. minihub-kconfig:: Web UI

Auth is nested under Web UI (same httpd login / Bearer path).

.. minihub-kconfig:: Auth

.. minihub-kconfig:: MCP

Cloud
-----

Vendor cloud bridges under ``Cloud`` in ``menuconfig``.

.. minihub-kconfig:: RainMaker

Firmware update
---------------

Host upgrade policy and the factory recovery image are siblings under
``Firmware update`` in ``menuconfig`` (OTA stages or writes the image; Vibe Mode
applies a staged image from external media).

.. minihub-kconfig:: OTA

.. minihub-kconfig:: Vibe Mode

Peripherals
-----------

On-board Hub hardware under ``Peripherals`` in ``menuconfig``.

.. minihub-kconfig:: USB MSC

What an OEM has to decide
-------------------------

Most symbols can stay at their default. These are the ones that are a product decision,
not a tuning knob:

* ``MINIHUB_VERSION``, ``MINIHUB_WEB_VERSION``, ``MINIHUB_LCD_UI_VERSION`` — the
  three release lines. See :doc:`versions`.
* ``MINIHUB_LOCAL_MAP`` — default off. On, the firmware includes geolocation
  and the map, and the Web and LCD consoles show it. Off, neither console
  offers a map and ``GET /api/location`` returns ``ok: false``.
* ``MINIHUB_NET_WIFI_STA`` / ``MINIHUB_NET_ETH`` — the transport. ``MINIHUB_LCD_UI``
  ``depends on !MINIHUB_NET_ETH`` because EMAC and the RGB LCD share pin groups on this
  board. ``MINIHUB_NET_WIFI_SOFTAP`` is for a product that drives Wi-Fi itself while
  ``MINIHUB_NET_WIFI_STA`` is off; an Ethernet-only Host sets it to ``n``. Wi-Fi
  never runs beside Zigbee or Thread, so an 802.15.4 product's transport here is
  Ethernet (:doc:`radios`).
* ``MINIHUB_WIFI_SSID`` / ``MINIHUB_WIFI_PASS`` — compiled into the image. Leaving the
  literal ``YOUR_SSID`` means "use NVS or provisioning"; putting a real network here writes
  its credentials into every unit's app binary, readable from flash unless flash encryption
  is on. See :doc:`security`.
* ``MINIHUB_WEB_LOGIN_AUTH_TOKEN`` — empty by default, and leave it that way unless every
  integrator of one deployment is meant to share one string: a compiled-in value beats the
  per-device NVS ``api_token`` and is identical on all units built from that recipe.
* ``MINIHUB_FACTORY_RESET_BUTTON`` — default off, so a stray BOOT press cannot erase a
  field unit. If you turn it on, ``FACTORY_RESET_GPIO`` / ``_HOLD_MS`` / ``_ACTIVE_LOW``
  become visible and the erase scope in :doc:`production` applies.
* ``MINIHUB_WEB_LOGIN_AUTH`` — default on. Off, LAN REST is open to anyone who can reach
  port 80.
* ``MINIHUB_STORE_URL`` — full URL (scheme + host, no trailing path) where a device looks
  for its catalog and firmware, e.g. ``https://minihub.espressif.com``. The default points at
  the public Espressif Store; the runtime value is set from the console or
  ``PUT /api/store/config`` and the compiled default is only the fallback. The device appends
  ``/api/catalog`` and ``/files/...`` to this base, so do not include a path such as
  ``/api/v1/plugins``.
* The radio-stack symbols (``MINIHUB_MATTER_ADAPTER``, ``MINIHUB_THREAD_ADAPTER``,
  ``MINIHUB_ZIGBEE_SDK``, ``MINIHUB_BLE_MESH_SDK``) — each pulls in a large component and
  changes what ``menuconfig`` can select, so they belong in a product's feature list rather
  than in every build. ``thread`` and ``zigbee`` cannot both be on.

Related
-------

* :doc:`host_customization` — making a product: ``application/<name>/``, its recipe, seeds
* :doc:`limits` — the fixed capacities that are *not* configurable
* :doc:`production` — flashing, partitions, factory reset scope
* :doc:`security` — what each default protects and what it assumes
* :doc:`api/errors` — what the device answers when a setting is refused

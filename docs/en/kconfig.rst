Kconfig Options
===============

:link_to_translation:`zh_CN:[中文]`

Every MiniHub build option in one place, generated at docs build time from the Kconfig
fragments themselves — not from a list someone keeps by hand. ``idf.py menuconfig`` shows
the same text; this page adds which file reads each symbol, so you can see whether
turning it on changes anything at all.

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

Three rules in that chain decide what a change does:

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
* Radio stacks that share IEEE 802.15.4 are refused, not silently combined: building
  ``thread`` and ``zigbee`` together stops with exit code 2 and tells you to pick one.

To see what a configured build actually uses, read the generated files rather than the
fragments: ``application/<app>/build/config/sdkconfig.h`` (what the C code sees),
``sdkconfig.cmake`` and ``sdkconfig.json`` next to it, or ``application/<app>/sdkconfig``
for the values that beat the defaults. ``tools/build_firmware.py`` prints the whole
resolved ``SDKCONFIG_DEFAULTS`` line before it invokes the build.

Core and memory
---------------

.. minihub-kconfig:: Core

.. minihub-kconfig:: Hub

.. minihub-kconfig:: JS plugins

.. minihub-kconfig:: Modules

Radios and adapters
-------------------

.. minihub-kconfig:: Networking

.. minihub-kconfig:: Provisioning

.. minihub-kconfig:: Matter

.. minihub-kconfig:: Thread

.. minihub-kconfig:: Zigbee

.. minihub-kconfig:: BLE Mesh

.. minihub-kconfig:: Modbus

.. minihub-kconfig:: MQTT adapter

Console, Store and access
-------------------------

.. minihub-kconfig:: Web UI

.. minihub-kconfig:: Auth

.. minihub-kconfig:: Store

.. minihub-kconfig:: MCP

.. minihub-kconfig:: RainMaker

Peripherals
-----------

.. minihub-kconfig:: LCD UI

.. minihub-kconfig:: RTSP camera

.. minihub-kconfig:: USB MSC

What an OEM has to decide
-------------------------

Most symbols can stay at their default. These are the ones that are a product decision,
not a tuning knob:

* ``MINIHUB_VERSION`` and ``MINIHUB_WEB_VERSION`` — the two release lines. See
  :doc:`versions`.
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

Host Customization
==================

:link_to_translation:`zh_CN:[中文]`

MiniHub Core is shared, and a shipped device differs from the generic Host in
only a handful of ways: the board it runs on, the features compiled in, and the
configuration it leaves the factory with. Put those differences in a product of
your own (OEM / secondary development). For the
layout of ``application/`` and of the generic ``minihub`` product, see
:doc:`application/index` · :doc:`application/minihub`.

Overview
--------

A **product** is a folder ``application/APP/``. Every difference lives there, and
none of them needs a change to Core:

* Board and partition table
* Sdkconfig feature set (Ethernet, Matter, Store, …)
* Default ``adapters.json`` / rules / scenes
* Optional factory-preinstalled Store modules

Factory flash and OTA: :doc:`production`.

Step-by-step: create ``myhub``
------------------------------

1. **Copy the generic Host as a template**

   .. code-block:: bash

      cp -a application/minihub application/myhub

   Keep Core in ``components/minihub_*``: a product that carries its own copy
   stops receiving Core fixes. The copied ``main.c`` usually needs no changes —
   Kconfig and the generated ``adapters_setup.generated.c`` select the stacks.

2. **Edit** ``application/myhub/manifest.yml``

   The copy already has a full ``sdkconfig_features`` list. Drop unused items
   (``rtsp_cam``, ``usb_msc``, …) to save flash. Minimal example:

   .. code-block:: yaml

      extends: _host_base
      board: esp32_s31_function_coreboard_1
      partition_table: partitions.csv
      store_preinstall: []
      sdkconfig_features:
        - common
        - memory
        - partitions_16m
        - eth
        - wifi_sta
        - dynamic_modules

3. **Edit** ``application/myhub/configs/sdkconfig/app.yml``

   This file alone is the feature list: sdkconfig composition and the generated
   component table both read ``app.yml``, and the manifest's
   ``sdkconfig_features`` is only a fallback for a product with no ``app.yml`` —
   so the two need not match word for word (the shipped ``minihub`` manifest
   lists one extra feature, ``rainmaker``). Each feature is a fragment and each
   fragment's options are listed, with what they actually set, in :doc:`kconfig`.

4. **Factory JSON** (optional)

   * ``application/myhub/configs/adapters.json``
   * ``application/myhub/configs/rules.json``, ``scenes.json``

   If a file is missing under the product, the build falls back to repo
   ``configs/*.json``.

   Type labels and widgets are **not** product JSON. They live in Host
   ``components/minihub_device_widget/model/*.csv``. Add a type there, or
   upload both CSVs via REST on a running Hub (``PUT /api/dev/catalog/…``; the
   Web console has no page for this). Device instances still
   register one point each (:doc:`data_model`; catalog: :doc:`type_catalog`).

5. **Add your own code**

   Add sources under ``application/myhub/main/`` (or a product component) and
   list them in ``CMakeLists.txt`` ``SRCS``. You usually leave bring-up as-is;
   stacks come from Kconfig.

   Which interface these sources use depends on where the code runs (the split is
   in :doc:`api/index` ); product code linked into this firmware mostly uses three
   pages:

   * :doc:`api/minihub_core` — point registry, state store, event bus, command routing
   * :doc:`api/minihub_adapter` — implementing ``minihub_adapter_t``, and the order
     Host calls it in
   * :doc:`api/net_api` — Host uplink (Ethernet / Wi-Fi STA) bring-up, IPv4 queries
     and DNS diagnostics

   Factory checks and automation scripts are not linked into the firmware; they drive
   the Hub over REST: :doc:`api/host_rest`.

   The device "firmware version" (OTA comparison, ``Firmwares/<chip>/<ver>/``)
   and the product/sidebar version (shown as ``v<ver>`` in the Web UI) are the
   SAME value, ``MINIHUB_VERSION`` (default ``0.1.0-dev``). Git describe still shows
   separately as the commit line.

   Only the Web bundle is versioned independently, via ``MINIHUB_WEB_VERSION``
   (``Webs/v<ver>/`` and ``window.mhWebVersion``). Web and firmware releases are
   usually not in sync, so set it separately in ``menuconfig`` (defaults to
   ``MINIHUB_VERSION``). ``tools/deploy.sh`` reads both ``MINIHUB_VERSION`` and
   ``MINIHUB_WEB_VERSION`` at publish time, and the Host build stamps the same
   value into the console it puts in ``storage``; nothing else in the tree
   carries a web version. See :doc:`versions`.

6. **Build and flash**

   .. code-block:: bash

      ./tools/build_firmware.py -app myhub build
      ./tools/build_firmware.py -app myhub -p PORT app-flash monitor

Hardware variants
-----------------

One product can carry more than one board recipe: put extra YAML files under
``application/<app>/configs/sdkconfig/variants/`` and switch between them with

.. code-block:: bash

   ./tools/apply_sdkconfig_variant.sh <name>       # variants/<name>.yml -> app.yml
   ./tools/apply_sdkconfig_variant.sh --restore    # variants/default.yml -> app.yml

A variant is a whole replacement for ``app.yml`` — the feature list, not a patch on top
of it. Whatever you put in ``app.yml`` by hand (a lab SSID, an extra feature) is what
gets overwritten.

The LCD firmware is the separate product ``application/minihub_lcd``
(``esp32_s31_korvo1``), built with ``-app minihub_lcd``. ``application/minihub``
is Ethernet Function CoreBoard only (``esp32_s31_function_coreboard_1``) — do not
put ``lcd`` in its ``app.yml`` or add a ``variants.lcd`` block.

A ``variants/<name>.yml`` file replaces ``app.yml`` features only. Component set,
board and ESP-IDF dependencies come from that product's ``manifest.yml`` (or, when
``app.yml`` includes the ``lcd`` feature, from a ``_<app>_lcd`` overlay profile if
one was merged in).

.. warning::

   ``apply_sdkconfig_variant.sh`` copies over ``application/<app>/configs/sdkconfig/app.yml``
   and keeps no backup: hand edits to the recipe are lost the moment you switch variants,
   and ``--restore`` does not return them either — it copies ``variants/default.yml``, which
   is a file you maintain, not the git history. Keep ``variants/default.yml`` in step with
   ``app.yml`` (they are identical in the shipped ``minihub`` product) and put anything you
   want to survive a switch into ``local.overrides``, which is not touched by this script.

Board glue
----------

Pin-level setup is not hand-written here. ``esp_board_manager`` owns the board, ``app.yml``
names it with ``board:``, and the build regenerates
``application/<app>/components/gen_bmgr_codes/`` from that definition:

.. code-block:: bash

   idf.py --preview bmgr -b esp32_s31_function_coreboard_1
   idf.py --preview bmgr -c ./boards -b <name>   # a board you define under application/<app>/boards/

ESP-IDF still treats S31 as a preview target, so every ``idf.py`` call needs ``--preview``
(``tools/build_firmware.py`` adds it). That generated directory is gitignored, and on a fresh
checkout it can still be the placeholder the build script lays down before Component Manager
has fetched ``esp_board_manager`` — which is why a first configure that cannot resolve
``REQUIRES: esp_board_manager`` is fixed by ``idf.py --preview reconfigure``, not by editing
those files: the next build overwrites anything you change there.

The board choices that reach into the product's configuration are Ethernet versus the RGB
LCD, which share pin groups (``MINIHUB_LCD_UI`` is declared ``depends on !MINIHUB_NET_ETH``),
and the PHY address, its reset GPIO and any chip-specific init. All of them are options
rather than constants — :doc:`kconfig` lists each with its default and the file that reads it.
Re-apply ``./tools/apply_component_patches.sh`` after any dependency refresh: a freshly
fetched ``managed_components`` silently undoes what is under ``patches/``.

Product file tree (typical)
---------------------------

.. code-block:: text

   application/myhub/
     manifest.yml
     partitions.csv
     configs/
       adapters.json
       rules.json
       scenes.json
       sdkconfig/
         app.yml
     main/
       CMakeLists.txt
       main.c
       adapters_setup.h
       idf_component.yml              # generated
       adapters_setup.generated.c   # generated
     boards/                          # optional board overrides

Partitions
----------

The generic Host ``partitions.csv`` is a starting point. Plan your own table
when a product has extra needs — for example RainMaker / Zigbee ``fctry`` NVS
size, OTA slot size, ``storage`` / ``modstore``. Zigbee stack NVRAM uses the
``fctry`` NVS partition (via ``storage_partition_name``); there is no separate
``zb_storage`` / ``zb_fct`` on the generic Host.

Web console
-----------

The console HTML is staged on ``storage`` as ``/data/web/``.
To skin ``index.html`` / ``login.html`` or push files with curl:
:doc:`web_console`. The generic ``CMakeLists.txt`` copies
``components/minihub_web/web/`` only (no ``application/APP/web/`` overlay).

Restyling the on-screen UI without a rebuild
--------------------------------------------

The LCD interface is a set of JSON documents the firmware reads at runtime from
``/data/ui/`` on the ``storage`` LittleFS partition. ``application/minihub/manifest.yml``
only decides whether the panel is built in (``CONFIG_MINIHUB_LCD_UI``). What the panel
shows is ``/data/ui/root.json`` and the files that document lists.

``root.json`` names the assets in load order::

    /data/ui/root.json
      constants/theme.json      colors, sizes
      assets/device_icons.json  icon ids referenced as ${image.<id>}
      templates/device_card.json
      screens/overview.json     screens/map.json  screens/store.json
      flows/main.json           screen list, initial screen, transitions

To change the on-screen UI:

- Edit a screen to change layout, styles, or which parameters appear.
- To add a screen, add a file, list it under ``assets``, add one transition in
  ``flows/main.json``, and add one ``nav_*`` action so the screen is reachable.
- Push the result by flashing the ``storage`` image, or during development with
  ``CONFIG_MINIHUB_DEV_FS_UPLOAD`` and ``PUT /api/dev/fs/ui/<path>``. That route
  accepts ``ui/`` only and caps one upload at 2 MiB.

Two checks keep an OEM edit within the firmware contract. Both run at build time:

- ``tools/test_lcd_ui_contract.py`` checks ``ui/**/*.json`` against
  ``components/minihub_lcd/ui_contract.json``. Every view path and action name the
  firmware reads must exist in the JSON. ``root.json`` must carry the version the
  contract expects. The device-slot count must match the template. A rename that
  the firmware does not know about fails ``idf.py`` configure, not the panel.
- ``CONFIG_MINIHUB_STORAGE_MIN_FREE_BYTES`` (checked in the Store install path)
  refuses a package that would eat the headroom the UI and device state need.

Device names and other free text come from the network, so they are not limited
to the built-in glyph set. ``ui/fonts/`` holds the optional FreeType face used
for those. UI chrome renders with LVGL's built-in Han Sans and stays readable
when that file is absent.

What the panel is **not**: it is not a hub point. It does not appear in the type
catalog, cannot be targeted by a rule, and has no device card of its own.

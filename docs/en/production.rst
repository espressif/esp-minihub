Deployment and production
=========================

:link_to_translation:`zh_CN:[中文]`

OEM release path: factory image, field OTA, and Store packages. First lab
bring-up: :doc:`getting_started`. Product folder and
partition planning: :doc:`host_customization`.

Do not bake lab SSID/PSK (``local.overrides``, gitignored) into a production
image. Do not put cloud passwords in factory ``adapters.json``.

Factory flash
-------------

From the repository root:

.. code-block:: bash

   # Lab: app only (keeps existing /data and /store)
   ./tools/build_firmware.py -app minihub -p PORT app-flash monitor

   # Full device: rewrite partitions listed in flasher_args
   ./tools/build_firmware.py -app minihub -p PORT flash

   # Store partition only
   ./tools/build_firmware.py -app minihub -p PORT modstore-flash

   # Host storage only (config + Web console under /data/web)
   ./tools/build_firmware.py -app minihub -p PORT storage-flash

   # Build then merge a single file (successful build also packs .mhub)
   ./tools/build_firmware.py -app minihub build merge

``build merge`` writes ``application/minihub/build/minihub_merged.bin`` for jig
flashing. A successful ``build`` is required first.

Partitions (generic minihub, 16 MB)
------------------------------------

``application/minihub/partitions.csv`` is a starting point, not a spec for every
SKU. Plan your own table in :doc:`host_customization`.

The current generic table includes:

* ``nvs`` — Wi-Fi creds, Store URL, auth, OTA metadata
* ``otadata`` / ``phy_init`` — OTA state and RF calibration
* ``ota_0`` / ``ota_1`` — dual app slots (5 MiB each)
* ``fctry`` — RainMaker claim / factory NVS; Zigbee stack NVRAM shares this
  partition (40 KiB). Changing the partition table requires a full flash.
* ``storage`` — LittleFS ``/data`` (``adapters.json``, rules, scenes; Host Web
  console under ``/data/web/``). Replacing Web files: :doc:`web_console`.
* ``modstore`` — LittleFS ``/store`` (installed ``.mhub``, factory preinstall
  quirks).

The generic Host does **not** ship ``zb_storage`` or ``zb_fct``. ESP Zigbee SDK
2.x stores NVRAM in NVS via ``storage_partition_name`` (MiniHub points it at
``fctry``). ``zb_fct`` (1 KiB FAT) was only for optional mfg-tool production
config on end devices; the coordinator Host does not use it. OEMs that need
Zigbee mfg production data must add their own FAT partition or use the Zigbee
secur / descriptor APIs.

Matter builds need large app slots. If the linker says the app does not fit,
grow ``ota_0`` / ``ota_1`` — do not shrink ``modstore`` first.

Factory Store preinstall
------------------------

List module ids that exist under ``examples/plugins/ID/`` (or
ids pulled in via ``MINIHUB_PLUGIN_DIRS``) in
``application/APP/manifest.yml``. The ``preinstall`` field in ``module.json`` is
ignored; see :ref:`store-preinstall`. Packing: :doc:`plugins/pack_and_catalog`.

.. code-block:: yaml

   store_preinstall:
     - color_bulb_js

Or flash ``minihub_merged.bin`` / ``storage-flash`` / ``modstore-flash`` on the jig.

.. only:: not hide_l3

   In-tree L3 needs ``CONFIG_MINIHUB_DYNAMIC_MODULES`` and ``build`` (emits the
   ``.so`` and packs). When artifacts already exist:

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub pack
      ./tools/build_firmware.py -app minihub -p PORT flash

   The JS id ``color_bulb_js`` above is listed under ``store_preinstall`` the
   same way.

Provisioning at scale
---------------------

1. Flash the factory image (no STA creds in NVS)
2. First boot: Ethernet if cabled; else SoftAP/BLE provisioning when
   ``CONFIG_MINIHUB_WIFI_PROVISIONING`` is on — see :doc:`networking`
3. Operator opens ``http://HUB/``, completes Wi-Fi (if needed) and Store login
4. Optional: set the production Store URL (``POST /api/store/config``; see
   :doc:`api/plugin_rest`)

   .. code-block:: json

      { "custom_url": "https://store.example", "disable_default": true }

Factory reset
-------------

When ``factory_reset_gpio61`` (or your product equivalent) is in
``sdkconfig_features``, holding the configured GPIO erases NVS and reboots.
LittleFS ``/data`` and ``/store`` are not wiped — use a full ``flash`` if the jig
needs a clean filesystem.

The public ``minihub`` sdkconfig does not enable secure boot, flash encryption,
or NVS encryption. An OEM may turn those on in the product sdkconfig; follow
the ESP-IDF security guide for that chip. This guide does not set a factory
policy for eFuse, serial number, or digital signature.

OTA in the field
----------------

Dual-slot OTA (``ota_0`` / ``ota_1``) writes the inactive slot, then reboots.
If the new app reaches ``minihub_ota_init``, rollback is cancelled; otherwise
the bootloader returns to the previous slot. Details: :doc:`ota`.

Store vs Host
-------------

.. only:: not hide_l3

   An L3 ``.so`` resolves Host symbols through the Plugin SDK and export
   allowlist; it is not tied to a particular firmware build. ``module.json``
   ``abi_version`` and ``core_api_min`` / ``core_api_max`` must bracket the running
   Host, and the chip architecture must match. You do not rebuild modules for every
   firmware release. There is no separate Host-version table: a module fits when
   those three fields bracket the running Host. Contract: :doc:`api/module_abi`.

The field catalog is still ``catalog.json`` + ``.mhub``; factory preinstall is
above.

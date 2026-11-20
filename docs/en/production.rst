Deployment and production
=========================

:link_to_translation:`zh_CN:[中文]`

The factory image, field updates, and the Store packages that go with them make
up the release path. First lab bring-up is :doc:`getting_started`; the product
folder and its partition planning are :doc:`host_customization`.

Lab Wi-Fi credentials belong in ``local.overrides``, the gitignored sdkconfig
fragment at ``application/APP/configs/sdkconfig/``. The build appends it last,
and the SSID/PSK then reach every unit flashed from that image.

Cloud account passwords for a vendor service do not belong in the generic
factory ``adapters.json`` either. After the user logs in, the module keeps the
credential in its own storage (NVS or a module-private file).

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

``build merge`` writes ``application/<app>/build/<app>_merged.bin`` (for this
product: ``minihub_merged.bin``) for jig flashing. A successful ``build`` is
required first. The same naming is used for ``minihub_lcd`` and other products.

Release logs
------------

``./tools/build_firmware.py --release build`` loads
``configs/sdkconfig/feature/release.defaults`` and rewrites the product
``sdkconfig`` so the app, bootloader, and maximum log level are Warning.
Adding ``release`` to the product ``app.yml`` ``features`` list does the same
on every build. ``CONFIG_MINIHUB_RELEASE`` selects those levels in Kconfig.
A saved ``sdkconfig`` that already has ``LOG_DEFAULT_LEVEL_INFO`` stays at
Info until this rewrite runs.

Partitions (generic minihub, 16 MB)
------------------------------------

The generic Host table is a starting point, not a spec for every SKU. Plan your
own table in :doc:`host_customization`. **Sizes and offsets** live only in the
CSV — open the file rather than copying numbers from this page:

* External-storage OTA (default):
  `application/minihub/partitions.csv <https://gitlab.espressif.cn/ae_group/minihub/esp-minihub/-/blob/master/application/minihub/partitions.csv>`__
  (``minihub_lcd`` uses the same map under
  `application/minihub_lcd/partitions.csv <https://gitlab.espressif.cn/ae_group/minihub/esp-minihub/-/blob/master/application/minihub_lcd/partitions.csv>`__)
* Internal dual-slot A/B (when ``CONFIG_MINIHUB_OTA_EXTERNAL_STORAGE`` is off):
  `application/minihub/partitions_int.csv <https://gitlab.espressif.cn/ae_group/minihub/esp-minihub/-/blob/master/application/minihub/partitions_int.csv>`__

Roles in the generic external table:

* ``nvs`` — Wi-Fi creds, Store URL, auth, OTA metadata, Vibe Mode hand-off state
* ``otadata`` / ``phy_init`` — boot selection and RF calibration
* ``ota_0`` — running app slot (external layout has only this; internal layout
  also has ``ota_1``)
* ``vibe_mode`` — ``app,factory`` maintenance image. With external-storage OTA it
  copies a staged image from LittleFS media into ``ota_0``; details in :doc:`ota`.
* ``fctry`` — RainMaker claim / factory NVS; Zigbee stack NVRAM shares this
  partition. Changing the partition table requires a full flash.
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

Matter builds need a large app slot. If the linker says the app does not fit,
grow ``ota_0`` and move ``vibe_mode`` and every later partition accordingly —
do not shrink ``modstore`` first. A changed table requires a full flash; the
Host ``vibe_mode`` CMake target (``components/minihub_vibe/subproject``) rebuilds
the factory image at the new offset on the next ``idf.py build``.

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

   In-tree ELF needs ``CONFIG_MINIHUB_DYNAMIC_MODULES`` and ``build`` (emits the
   ``.so`` and packs). When artifacts already exist:

   .. code-block:: bash

      ./tools/build_firmware.py -app minihub pack
      ./tools/build_firmware.py -app minihub -p PORT flash

   The JS id ``color_bulb_js`` above is listed under ``store_preinstall`` the
   same way.

Provisioning at scale
---------------------

1. Flash the factory image (no STA creds in NVS)
2. First boot: Ethernet if cabled; else SoftAP provisioning when
   ``CONFIG_MINIHUB_WIFI_PROVISIONING`` is on — see :doc:`networking`
3. Operator opens ``http://HUB/``, signs in as ``admin`` (setting the password on
   first visit), and completes Wi-Fi if needed
4. Optional: set the production Store URL (``POST /api/store/config``; see
   :doc:`api/plugin_rest`)

   .. code-block:: JSON

      { "custom_url": "https://store.example", "disable_default": true }

Factory reset
-------------

When ``factory_reset_gpio61`` (or your product equivalent) is in
``sdkconfig_features``, holding the configured GPIO performs a factory reset and
reboots. It removes user data; it does not reformat the filesystems:

* ``/data/config/rules.json``, ``scenes.json`` and ``adapters.json``
  (:file:`components/minihub_config/minihub_config.c:602`)
* every Store-installed module and pack under ``/store``
  (:file:`components/minihub_store/minihub_store.c:2140`)
* NVS as a whole, which takes the Wi-Fi credentials, the custom Store URL and the
  console login password with it (:file:`components/minihub_provision/minihub_factory_reset.c:34`)

Factory preinstall (``/store/preinstall``) and the Web console (``/data/web``) are
deliberately left alone, and LittleFS ``/data`` and ``/store`` keep their other
contents — use a full ``flash`` if the jig needs a clean filesystem.

.. warning::

   After a reset there is **no console password** until someone sets one: the login
   hash lives in NVS and is erased with everything else, and
   ``POST /api/auth/setup`` answers without authentication by design so that a fresh
   device can be configured. The first person to open the console on a reset unit owns
   it. Treat a reset device as insecure until its own password is set, and have the
   factory set one rather than shipping units into the field in setup state.

The public ``minihub`` sdkconfig does not enable secure boot, flash encryption,
or NVS encryption. An OEM may turn those on in the product sdkconfig; follow
the ESP-IDF security guide for that chip. This guide does not set a factory
policy for eFuse, serial number, or digital signature.

SBOM and vulnerability scanning
--------------------------------

The MiniHub tree is Apache-2.0 and built from source, so supply-chain and
first-party checks belong in your pipeline. Two layers:

SBOM and dependency CVEs
~~~~~~~~~~~~~~~~~~~~~~~~~

`esp-idf-sbom <https://github.com/espressif/esp-idf-sbom>`_ turns a built
project into an SPDX or CycloneDX bill of materials and checks it against the
NVD for known CVEs. Install it, build the firmware, then run:

.. code-block:: bash

   pip install esp-idf-sbom
   ./tools/build_firmware.py --app minihub build
   esp-idf-sbom create -o minihub.spdx \
       application/minihub/build/project_description.json
   esp-idf-sbom check minihub.spdx

``check`` flags CVEs in ESP-IDF and managed components. MiniHub's own components
(``components/minihub_*``, ``application/minihub/main``) appear in the SBOM as
packages, but they carry no SBOM metadata and are absent from the NVD, so
``check`` will not report CVEs in first-party code. To make those packages
describable, add an ``sbom.yml`` (``version`` / ``license`` / ``supplier``) at
the project root and in each first-party component.

First-party code scanning
~~~~~~~~~~~~~~~~~~~~~~~~~~

First-party code is not in any CVE database, so scan it with static and dynamic
analysis instead:

* **clang-tidy** (free, Apache-2.0) — the IDF build writes
  ``application/minihub/build/compile_commands.json``; run clang-tidy against it
  with a ``.clang-tidy`` config at the repo root.

  The recommended path is the integrated ``lint`` command, which auto-discovers
  the repo-root ``.clang-tidy`` and scans only first-party sources (it skips the
  IDF framework, ``managed_components`` and the generated ``build/`` tree):

  .. code-block:: bash

     ./tools/build_firmware.py lint                 # all first-party sources
     ./tools/build_firmware.py lint --lint          # lint automatically after build
     ./tools/build_firmware.py lint -j 8 path/to/file.c   # one file, 8 jobs

  **Toolchain**: prefer the ESP-patched clang — ``idf_tools.py install esp-clang``
  (its ``clang-tidy`` is auto-detected under ``~/.espressif/tools/esp-clang`` and
  understands the ``esp32s31`` RISC-V target and ESP newlib). A stock LLVM
  clang-tidy also works: ``lint`` then sanitises the ``compile_commands.json``
  (rewrites ``-march``/``-mcpu``/``-mtune``, drops GCC-only flags, expands
  ``@file`` response files and adds the toolchain newlib include dirs) so the
  frontend can parse the sources — but it still cannot fully parse ESP newlib
  system headers, so expect some compiler errors from vendored headers.

  Or invoke clang-tidy directly (it also picks up the root ``.clang-tidy``):

  .. code-block:: bash

     clang-tidy -p application/minihub/build \
         components/minihub_core/src/minihub_core.c

  LLVM's ``run-clang-tidy`` can also scan ``components/minihub_*`` and
  ``application/minihub/main``. A root ``.clang-tidy`` enabling
  ``bugprone-*``, ``clang-analyzer-*``, ``cert-*`` and
  ``cppcoreguidelines-*`` is provided.

* **Fuzzing** (free, open source — LibFuzzer, AFL/AFL++) — target the untrusted
  parsers: REST/WebSocket JSON, ``adapters.json`` / catalog parsing, and web
  uploads.

* **CodeQL** — free for open-source and research use only; closed-source or
  commercial products need a GitHub Advanced Security / commercial license. The
  Apache-2.0 MiniHub repository itself may use it at no cost.

OTA in the field
----------------

A field update stages the image on external LittleFS media, then Vibe Mode
(``app,factory``) copies it into ``ota_0``. There is one running slot, so there
is no inactive slot to fall back to; recovery is Vibe Mode copying the staged
image again. Confirmation happens in ESP-IDF start-up, just before ``app_main``.
An image that reaches ``app_main`` is kept even if it misbehaves afterwards.
Details: :doc:`ota`.

Store vs Host
-------------

.. only:: not hide_l3

   An ELF ``.so`` resolves Host symbols through the Plugin SDK and export
   allowlist; it is not tied to a particular firmware build. ``module.json``
   ``abi_version`` and ``core_api_min`` / ``core_api_max`` must bracket the running
   Host, and the chip architecture must match. You do not rebuild modules for every
   firmware release. There is no separate Host-version table: a module fits when
   those three fields bracket the running Host. Contract: :doc:`api/module_abi`.

The field catalog is still ``catalog.json`` + ``.mhub``; factory preinstall is
above.

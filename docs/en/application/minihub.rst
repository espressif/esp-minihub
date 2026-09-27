minihub
=======

:link_to_translation:`zh_CN:[中文]`

Generic product: ``application/minihub/``. Select with ``-app minihub`` or
``MINIHUB_APP=minihub``. Shared product vs Core rules: :doc:`index`.

Role
----

Default options are Ethernet-oriented (Wi-Fi STA fallback, USB MSC, RTSP, Modbus,
and related features). Reference board ``esp32_s31_function_coreboard_1``.

::

   ./tools/build_firmware.py -app minihub build

LCD variant
-----------

On-device LCD UI is ``CONFIG_MINIHUB_LCD_UI`` (refresh interval
``CONFIG_MINIHUB_LCD_UI_REFRESH_MS``; optional ``CONFIG_MINIHUB_DEV_FS_UPLOAD``
to PUT UI JSON). The option ``depends on !MINIHUB_NET_ETH``.

Directory layout
----------------

.. code-block:: text

   application/minihub/
     manifest.yml
     partitions.csv
     CMakeLists.txt
     README.md
     configs/
       adapters.json
       sdkconfig/
         app.yml
         feature/*.defaults
         variants/
         local.overrides.example
     main/
       main.c
       CMakeLists.txt
       Kconfig.projbuild
       adapters_setup.h
       adapters_setup.generated.c
       idf_component.yml

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Path
     - Role
   * - ``manifest.yml``
     - Product profile: board, partition table, ``sdkconfig_features``,
       ``store_preinstall``, ``variants``. Merged with ``_host_base`` in repo
       ``configs/minihub_manifest.yml`` at compile time, which then writes
       ``main/idf_component.yml`` and other generated files (see :doc:`index`).
       Paths in this table are under ``application/minihub/``. They are not the
       repo-root ``configs/`` tree. A missing product JSON file falls back to
       repo ``configs/*.json`` (:doc:`../host_customization`).
   * - ``partitions.csv``
     - Flash map (``nvs`` / ``ota`` / ``storage`` / ``modstore``, …). A starting
       point, not every SKU; see :doc:`../production`.
   * - ``CMakeLists.txt``
     - IDF project: adds repo ``components/`` and
       ``adapters/`` to ``EXTRA_COMPONENT_DIRS``; Store plugins use
       ``MINIHUB_PLUGIN_DIRS`` (``examples/plugins``), not a Host builtin tree;
       optional Matter / RainMaker from the environment.
   * - ``README.md``
     - Lab build notes for this product (``-app minihub``, LCD option).
   * - ``configs/adapters.json``
     - Factory adapters JSON copied into LittleFS ``/data/config/``. If missing,
       the build falls back to repo ``configs/adapters.json``.
   * - ``configs/rules.json`` / ``scenes.json``
     - Not present on the generic product; the build uses repo ``configs/``. An
       OEM tree may override them under the product ``configs/``.
   * - ``configs/sdkconfig/app.yml``
     - Compose options (board + ``features``). Must match manifest
       ``sdkconfig_features``.
   * - ``configs/sdkconfig/feature/*.defaults``
     - Product-local Kconfig fragments (USB MSC, RTSP, Modbus, …). Shared
       fragments live in repo ``configs/sdkconfig/``.
   * - ``configs/sdkconfig/variants/``
     - Enable ``CONFIG_MINIHUB_LCD_UI``.
   * - ``configs/sdkconfig/local.overrides.example``
     - Copy to ``local.overrides`` (gitignored) for lab Wi-Fi; do not commit.
   * - ``main/main.c``
     - ``app_main``: NVS, networking, Hub, Web. You usually leave it; stacks come
       from Kconfig and generated ``adapters_setup.generated.c``.
   * - ``main/CMakeLists.txt``
     - Product sources, LittleFS factory image, Web preinstall copy.
   * - ``main/Kconfig.projbuild``
     - Product Kconfig: Wi-Fi, provisioning, Web login, adapter switches.
   * - ``main/adapters_setup.h``
     - Declares ``minihub_adapters_register_all()``.
   * - ``main/adapters_setup.generated.c``
     - Auto-generated; do not hand-edit.
   * - ``main/idf_component.yml``
     - Auto-generated; do not hand-edit.

The build also writes ``sdkconfig`` and ``build/`` here; do not treat them as
sources. RainMaker sources live in the optional submodule
``components/esp-rainmaker/``.

After editing ``manifest.yml``, just build (``build_firmware.py`` merges the
manifest). ``store_preinstall`` lists module ids under
``examples/plugins/`` (plus ``MINIHUB_PLUGIN_DIRS``) packed into the
factory ``modstore`` image. Public Host keeps the list empty unless you
intentionally factory-bundle modules.

Copy this product to a new folder: :doc:`../host_customization`. Factory flash
and partitions: :doc:`../production`.

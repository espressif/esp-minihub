Application Products
====================

:link_to_translation:`zh_CN:[中文]`

Each folder under ``application/`` is one ESP-IDF firmware **product**. The
chapters below are those products. Read this page before
:doc:`../host_customization` (how to copy a product and ship an OEM SKU).

.. toctree::
   :maxdepth: 1

   minihub

.. rubric:: What a product is

MiniHub splits **product** from **Core**:

* ``application/APP/`` — the Host you flash: board, partition table, sdkconfig
  recipe, factory JSON, ``app_main``, and product Kconfig. Folder name is the
  product name (``APP``).
* ``components/`` — shared Core and protocol adapters. Store sources
  live in ``examples/plugins/`` or ``MINIHUB_PLUGIN_DIRS``. Every product links
  Core and adapters; do not copy them into
  ``application/APP/``.
* Repo ``config/`` — shared manifest base (``_host_base`` in
  ``config/minihub_manifest.yml``) and shared sdkconfig fragments.

Selecting a product: ``-app APP`` or ``MINIHUB_APP=APP`` (default ``minihub``).
The wrapper applies board defaults and runs the IDF build under
``application/APP/``. See :doc:`../getting_started`.

.. rubric:: Typical files

.. code-block:: text

   application/APP/
     manifest.yml
     partitions.csv
     CMakeLists.txt
     config/
       adapters.json
       sdkconfig/app.yml
     main/
       main.c
       CMakeLists.txt
       Kconfig.projbuild
       adapters_setup.h
       adapters_setup.generated.c   Generated; do not hand-edit
       idf_component.yml           Generated; do not hand-edit

``rules.json`` / ``scenes.json`` may live under the product ``config/`` or fall
back to repo ``config/``.

``tools/sync_host_manifest.py`` merges the product
``application/APP/manifest.yml`` (``extends: _host_base``) with the shared
``_host_base`` profile in repo ``config/minihub_manifest.yml``, then writes
generated files the IDF build consumes (do not hand-edit):

* ``application/APP/main/idf_component.yml``
* ``application/APP/main/adapters_setup.generated.c``
* ``cmake/generated/minihub_components.cmake`` (and a few ``*.txt`` in that folder)

``./tools/build_firmware.py`` already runs this before compile.

``sdkconfig_features`` must match ``config/sdkconfig/app.yml`` ``features:``.
Compose: ``tools/compose_sdkconfig.py``. Stack list:
``cmake/generated/sdkconfig_stacks.gen``.

.. rubric:: What belongs in the product vs Core

Put in ``application/APP/``:

* Board, partitions, factory JSON, sdkconfig features
* Extra sources listed in product ``main/CMakeLists.txt``
* Product-only Kconfig (``main/Kconfig.projbuild``)

Keep in ``components/`` (or Store plugins):

* Shared Hub services and adapters
* Vendor clouds, OEM device catalogs, cloud tokens — **not** in public
  ``config/`` seeds or Core. Ship as ``.mhub`` or a private product tree.

Web console files come from ``components/minihub_web/web/`` into
``/store/preinstall/web/``. There is no ``application/APP/web/`` overlay unless
you change that product's ``CMakeLists.txt``. See :doc:`../web_console`.

When another generic product lands under ``application/``, add a sibling RST next
to ``minihub.rst`` and list it in the toctree above. Private OEM copies do not
need a chapter here; follow :doc:`../host_customization`.

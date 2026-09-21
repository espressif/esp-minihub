Contributing
============

:link_to_translation:`zh_CN:[中文]`

Thank you for improving MiniHub. This project is intended to be **open and
document-driven**: secondary developers should rely on the Programming Guide
and public headers, not on reading Core internals.

License
-------

Contributions are accepted under the same **Apache License 2.0** as the
repository (see ``LICENSE`` at the repo root).

What belongs where
------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Change type
     - Location / policy
   * - OEM Host product (board, partitions, factory JSON)
     - ``application/APP/`` — see :doc:`application/index` then :doc:`host_customization`
   * - Vendor cloud / ecosystem logic
     - ``components/minihub_plugins/ID/`` or ``examples/plugins/ID/`` as Store
       ``.mhub`` — **not** Host Core
   * - Shared Host Core fix
     - ``components/`` ``minihub_``-prefixed libs, ``adapters/`` — keep ecosystem-agnostic
   * - Device type catalog (labels / widgets)
     - ``components/minihub_device_widget/model/*.csv`` — one device = one
       point; no per-type C structs. See :doc:`type_catalog`
   * - User-facing documentation
     - ``docs/en/`` and matching ``docs/zh_CN/`` (same tree)
   * - Module ABI / export list
     - ``docs/module_sdk/``, :doc:`api/module_abi`

Do **not** add vendor cloud tokens, default OEM secrets, or vendor-only rules to
generic Host ``config/`` factory JSON.

Git workflow
------------

* **Commits:** conventional commits (``feat:``, ``fix:``, ``docs:``, …)
* **Branches:** use underscores, not hyphens — e.g. ``feature/modbus_tcp_client``
  (see repository ``AGENTS.md`` and :doc:`vibe_coding`)
* **Author:** human ``user.name`` / ``user.email``; no AI co-author trailers in commits

Documentation contributions
---------------------------

1. Edit RST under ``docs/en/`` **and** ``docs/zh_CN/`` (paths must stay in sync;
   CI compares both trees)
2. Build locally (output ``docs/_build/``):

   .. code-block:: bash

      ./tools/build_docs.sh

   Or ``cd docs`` then ``build-docs -bs html -t esp32s31 -l en`` (and ``zh_CN``).
   Do not run ``build-docs --source-dir docs`` from the repo root.
   GitLab job ``docs:html`` (``pre_check``) builds English and Chinese and fails on
   Sphinx warnings.

3. Sphinx warnings fail the build — avoid angle-bracket placeholders inside backtick
   literals; use ``APP`` or ``ID`` instead
4. CJK titles need underlines at least as wide as the title (East-Asian width)

See ``docs/README.md`` for layout. Internal notes live in ``docs/superpowers/`` and
are not part of the published toctree.

Code and firmware
-----------------

* Build reference Host: ``./tools/build_firmware.py -app minihub build``
* After ``manifest.yml`` edits, just build (``build_firmware.py`` merges)
* L3 modules: ``./tools/build_firmware.py -app minihub modules`` (compile every native ``.so``; pack with ``pack``)
* Standalone L3 demos (no IDF project): ``python3 tools/test_build_plugin_standalone.py -v``
  (GitLab job ``build:plugin_standalone``; needs ``riscv32-esp-elf-gcc`` and ``cmake``)
* CI: ``.gitlab-ci.yml``

When changing Host symbols visible to L3 modules, update
``docs/module_sdk/host_export_allowlist.json`` and run
``python3 tools/gen_host_export_symbols.py`` as documented in
:doc:`plugins/develop_l3`.

Review expectations
-------------------

* Host Core changes should not introduce vendor-specific defaults
* Plugin changes should document new ``module.json`` fields or REST RPC in the
  Programming Guide or :doc:`api/plugin_rest`
* ABI bumps must update ``abi_version`` / ``core_api_*`` docs and pack notes in
  :doc:`production`

Questions
---------

Use the project issue tracker for bugs and feature requests. For architecture
questions, start with :doc:`introduction` and :doc:`architecture` — if the guide
is unclear, a documentation fix is often the right contribution.

PC-side Tools
==============

:link_to_translation:`zh_CN:[中文]`

Every runnable script under ``tools/`` is listed below: what it does, how to
invoke it, and where this guide documents it — ``this page only`` means the table
below is the only description. Common sequences are covered in
``tools/README.md`` in the repository; the sections below are the index.

Two environment variables are shared by most of them:

* ``MINIHUB_APP`` — which product under ``application/`` you are building or publishing.
  Default ``minihub``. Same value as ``-app``.
* ``MINIHUB_PLUGINS_DEPLOY_DIR`` — the deploy root that pack, publish and serve scripts
  write into. Default ``~/workspace/minihub_store``. One tree, one unit per folder;
  its layout is :doc:`api/deploy_index`.

Build a product
---------------

.. minihub-tools:: Build a product

Pack and publish
----------------

.. minihub-tools:: Pack and publish

Check and gate
--------------

These run as part of a build or a docs build; a failure here is a real defect, not a
lint preference. ``tools/ci/install_git_hooks.sh`` wires the commit-msg hook that keeps
tool attribution out of the history, and ``tools/git-hooks/`` holds the hook itself.

.. minihub-tools:: Check and gate

Work with a running Hub
-----------------------

.. minihub-tools:: Work with a running Hub

For plugin authors
------------------

``minihub_plugin_mcp.py`` is the stdio MCP server for an editor (Cursor, VS Code):
scaffold, validate and pack a plugin from the IDE. It is **not** the Hub's own on-device
MCP server that agents talk to (:doc:`api/mcp`); the two have nothing in common but the
protocol. Its implementation lives in ``tools/plugin_mcp/``.

.. minihub-tools:: For plugin authors

The guide itself
----------------

.. minihub-tools:: The guide itself

``docs/`` is built by ``build_docs.sh`` for both languages at once, warnings as errors,
with the en/zh file trees compared.

Related
-------

* :doc:`getting_started` — the first build, step by step
* :doc:`host_customization`, :doc:`kconfig` — making a product and its options
* :doc:`plugins/overview` — what a plugin is before you scaffold one
* :doc:`api/deploy_index` — the deploy root these scripts fill in
* :doc:`production` — what a flashing jig needs from ``Firmwares/``

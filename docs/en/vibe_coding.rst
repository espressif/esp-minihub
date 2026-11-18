Vibe coding
===========

:link_to_translation:`zh_CN:[中文]`

MiniHub can be worked on with an AI coding assistant: an OEM product under
``application/``, a Store plugin, or a Hub that is already running and answers
REST on the LAN. The assistant is an optional accelerator, not a prerequisite;
confirm the same flow by hand once, then let the agent repeat it. The sections
below describe how to set that up.

Where the agent reads its instructions
--------------------------------------

Open the repository root as the workspace. These are the files the agent works
from, and any of them can also be handed to it directly:

* ``AGENTS.md`` (repository root) — the entry point for every assistant. It
  holds the docs map, the git rules, the Host constraints and the core data
  model, and it wins over IDE-only configuration when the two disagree.
* ``.agent/skills/<name>/SKILL.md`` — one project skill per kind of task. Have
  the agent read the matching skill before it edits Host code, a Store plugin,
  or this guide: ``minihub-host`` (product, Kconfig, stacks), ``minihub-docs``
  (this guide), and one ``minihub-plugin-*`` skill per plugin tier.
* ``docs/en/`` and ``docs/zh_CN/`` — this guide.

``.cursor/skills`` is a symlink to ``.agent/skills/``, so a tool that looks in
its own directory still reads the same files.

Point it at the right chapter
-----------------------------

Give the agent the chapter for the task. Customization belongs in a product or a
plugin — start there, not in the Core ``.c`` files:

* Architecture and points: :doc:`architecture`, :doc:`type_catalog`,
  :doc:`data_model` (one device = one point; catalog CSV for widgets)
* Radios and access: :doc:`radios`
* Build and flash: :doc:`getting_started`
* OEM Host: :doc:`host_customization`, :doc:`production`
* Store plugins: :doc:`plugins/overview` (pick JSON or JS, then that chapter)
* REST / MCP tool list: :doc:`api/host_rest`, :doc:`api/mcp`
* Git and contributions: :doc:`contributing`

What the assistant must follow
------------------------------

* Keep Host Core ecosystem-agnostic: no vendor cloud tokens, default adapters, or
  vendor rules in ``components/minihub_*`` or generic ``configs/``
* Third-party / demo plugins: ``examples/plugins/ID/`` or
  ``MINIHUB_PLUGIN_DIRS``; pack as ``.mhub``

IDE MCP
-------

Plugin authoring comes with its own MCP server. The repository points Cursor
(``.cursor/mcp.json``) and VS Code (``.vscode/mcp.json``) at
``python3 tools/minihub_plugin_mcp.py``, which offers ``plugin_guide``,
``scaffold_plugin``, ``validate_plugin`` and ``pack_plugin``; the configs
themselves live in ``.agent/mcp/`` and are symlinked into both directories. That
server runs on your computer and works against the source tree. The on-device
server described below is a separate thing: it talks to a Hub over the network.

On-device MCP
-------------

A Hub can serve MCP itself. Enable ``CONFIG_MINIHUB_MCP_SERVER`` and initialize
the ``components/esp-claw`` submodule; a local MCP client then reaches the Hub
over the transport the product wires up, and every tool answers with JSON text.

A tool can only act on a Hub that has finished booting, so a call made during
boot or just after ``hub.reload`` may come back ``HUB_NOT_READY``. ``hub.reload``
is additionally limited to one call per 5 s. The full tool list, including what
``hub.api`` is allowed to proxy, is in :doc:`api/mcp`.

The Hub serves twelve tools. A few cover most sessions: ``points.list`` and
``command.invoke`` read and drive points, ``store.config.get`` and
``store.config.set`` read and write the Store URL, and ``hub.api`` proxies
allowlisted REST calls (JSON GET/POST/PUT — the raw ``.bin`` firmware upload is
not on the allowlist, so it stays out of reach) — installing or removing a Store
module also goes through
``hub.api``, as ``POST /api/store/install`` and ``POST /api/store/uninstall``.

Without MCP, the same work goes over plain LAN at ``http://HUB/api/``
(:doc:`api/host_rest`).

Suggested loop
--------------

Typical loop: the agent reads the matching skill and chapter, edits, then
verify on the device — serial logs, ``GET /api/modules``, and
``POST /api/hub/reload`` when the change needs one.

.. only:: not hide_l3

   ELF undefined symbols must be on ``host_export_allowlist.json``; declarations
   come from ``minihub_plugin_sdk.h`` — :doc:`plugins/develop_elf`.

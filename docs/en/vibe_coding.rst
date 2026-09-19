Vibe coding
===========

:link_to_translation:`zh_CN:[中文]`

Use an AI coding assistant (IDE agent, CLI agent, or MCP client) to customize
MiniHub: Host products, Store plugins, or a live Hub via on-device MCP.

The Programming Guide and public headers are the convention. Open this repo as
the workspace; assistants read root ``AGENTS.md`` — no extra setup. If
``AGENTS.md`` conflicts with this guide, this guide wins.

Point the assistant at the right pages
---------------------------------------

Give it the repo root and the chapter for the task. Do not send it to Core
``.c`` files first:

* Architecture and points: :doc:`architecture`, :doc:`type_catalog`,
  :doc:`data_model` (one device = one point; catalog CSV for widgets)
* Radios and access: :doc:`radios`
* Build and flash: :doc:`getting_started`
* OEM Host: :doc:`host_customization`, :doc:`production`
* Store plugins: :doc:`plugins/overview` (pick L1 / L2 / L3, then that chapter)
* REST / MCP tool list: :doc:`api/host_rest`, :doc:`api/mcp`
* Git and contributions: :doc:`contributing`

What the assistant must follow
------------------------------

* Keep Host Core ecosystem-agnostic: no vendor cloud tokens, default adapters, or
  vendor rules in ``components/minihub_*`` or generic ``config/``
* Vendor / demo plugins: ``components/minihub_plugins/ID/`` or
  ``examples/plugins/ID/`` plus ``MINIHUB_PLUGIN_DIRS``; pack as ``.mhub``
* Branch names use underscores, not hyphens (e.g. ``feature/modbus_tcp_client``)
* Commit author is the human ``user.name`` / ``user.email``; no AI co-author
  trailers

IDE MCP
-------

When writing Store plugins, this repo already wires Cursor
``.cursor/mcp.json`` and VS Code ``.vscode/mcp.json`` to
``python3 tools/minihub_plugin_mcp.py`` (tools: ``plugin_guide``,
``scaffold_plugin``, ``validate_plugin``, ``pack_plugin``). That is for the
IDE. It is not the on-device MCP below.

On-device MCP
-------------

Optional. Enable ``CONFIG_MINIHUB_MCP_SERVER`` (needs the ``components/esp-claw``
submodule). A local MCP client reaches the Hub through the product-wired MCP
transport. Tools return JSON text.

Most tools need the Hub ready; boot or ``hub.reload`` may return
``HUB_NOT_READY``. MCP ``hub.reload`` is limited to once per 5 s. Full tools and
the ``hub.api`` allowlist: :doc:`api/mcp`.

Typical use:

* ``points.list`` / ``command.invoke`` — inspect points and send commands
* ``hub.api`` — allowlisted REST proxy (JSON GET/POST/PUT; not multipart firmware
  upload)
* ``store.config.*`` and install tools — Store URL and modules (see the REST
  chapter)

Without MCP, the assistant can still use LAN ``http://HUB/api/``
(:doc:`api/host_rest`).

Suggested loop
--------------

1. Have the assistant read this chapter and the RST for the task, then edit
2. L3 undefined symbols must be on ``host_export_allowlist.json``; declarations
   come from ``minihub_plugin_sdk.h`` — :doc:`plugins/develop_l3`
3. Verify on device: serial logs, ``GET /api/modules``, ``POST /api/hub/reload``
   if needed

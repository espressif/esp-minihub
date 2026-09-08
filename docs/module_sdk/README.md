# Module SDK (Store plugins)

| Path | Purpose |
|------|---------|
| `host_export_allowlist.json` | Host ELF symbols L3 `.so` may leave undefined |
| `templates/l1/` | L1 pack (`inject` into mqtt) |
| `templates/l2/` | L2 JS (`main.js`) |
| `templates/l3/` | L3 native (`minihub_module_entry`) |

```bash
python3 tools/new_store_module.py --tier l2 --id my_js --name "My JS"
python3 tools/pack_store_module.py pack my_js
python3 tools/sync_store_catalog.py all
```

Do not copy an existing vendor plugin as a template. Agent workflow: `AGENTS.md` and `.cursor/skills/minihub-plugin-l*/`.

## IDE MCP (plugin authors)

Stdio server for Cursor / VS Code. It is **not** `CONFIG_MINIHUB_MCP_SERVER` (Hub tools for a running device).

```bash
python3 tools/minihub_plugin_mcp.py
```

This checkout already lists it in `.cursor/mcp.json` and `.vscode/mcp.json`.

| Tool | Purpose |
|------|---------|
| `plugin_guide` | L1 / L2 / L3 / `host.*` / pack rules |
| `scaffold_plugin` | Template files (optional write to `out_dir`) |
| `validate_plugin` | `module.json` id/kind/inject + L2 JS bans |
| `pack_plugin` | Zip L1/L2 into `.mhub` (L3 needs an existing `.so`) |

Plugin-only workspace (no Host clone): still run this script from an `esp-minihub` tree (or copy `tools/plugin_mcp/` + `docs/module_sdk/templates/` + `tools/minihub_plugin_mcp.py`), then `scaffold_plugin` with `out_dir` pointing at the customer's folder.

Tests: `python3 tools/plugin_mcp/test_plugin_mcp.py`.

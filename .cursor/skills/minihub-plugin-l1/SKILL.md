---
name: minihub-plugin-l1
description: Create or edit MiniHub Store L1 packs (module.json inject into an existing Host adapter, no main.js / no .so). Use when the user wants a config pack, inject devices into mqtt/zigbee/modbus, or kind pack.
---

# MiniHub Store L1 (pack)

L1 is **config only**: `module.json` (+ optional `inject` / `requires`). No `main.js`, no `CMakeLists.txt`.

## Scaffold

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
```

Or IDE MCP: `scaffold_plugin` with `tier=l1` (`python3 tools/minihub_plugin_mcp.py`).

## `module.json`

- `id` = directory name under `components/store_modules/`
- `kind`: `"pack"` (or omit if `runtime` is `"pack"` / `config_only: true`)
- `inject.adapter` must be a Host adapter that already exists (`mqtt`, `zigbee`, `ble_mesh`, `modbus`, …)
- Do not invent a new radio stack here — that is Host or L3

## Pack and install

```bash
python3 tools/pack_store_module.py pack my_pack
python3 tools/sync_store_catalog.py all
# Point Hub Store at the catalog, then refresh + install (REST: custom_url, not url)
```

Docs: `docs/plugins_and_store.md` or `docs/en/plugins/develop_l1.rst` / `pack_and_catalog.rst`.

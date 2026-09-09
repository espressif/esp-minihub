# Agent instructions (MiniHub)

For any AI coding assistant. Prefer this file and `docs/` over IDE-only config when they conflict.

Read the matching **project skill** under `.cursor/skills/` before editing Host, Store plugins, or docs.

## Docs map (use what exists)

| Topic | RST (preferred if present) | Markdown fallback |
|-------|----------------------------|-------------------|
| Build / product | `docs/en/getting_started.rst` · `docs/en/host_customization.rst` | `docs/build.md` |
| Plugins / Store | `docs/en/plugins/` | `docs/plugins_and_store.md` |
| Data model | `docs/en/data_model.rst` | product `config/*.json` |
| REST / JS / MCP / C ABI | `docs/en/api/` | headers under `components/*/include/` |
| Production | `docs/en/production.rst` | — |

Chinese mirrors: `docs/zh_CN/` (same relative paths as `docs/en/`).

## Git

- No tool attribution: no `Co-authored-by: Cursor`, no `Made-with: Cursor`.
- Conventional commits; author is `user.name` / `user.email`.
- Branch names: **underscores only** (no hyphens), e.g. `feature/modbus_tcp_client`.

## Vibe Coding tasks

| User wants | Skill | Do not |
|------------|--------|--------|
| New/OEM Host product, Kconfig, seeds, stacks | `.cursor/skills/minihub-host/` | Bake vendor cloud tokens into Core |
| L1 pack (inject into an existing adapter) | `.cursor/skills/minihub-plugin-l1/` | Add a new radio stack |
| L2 JS plugin (`main.js`) | `.cursor/skills/minihub-plugin-l2/` | Call `esp_wifi_*` from JS |
| L3 native `.so` | `.cursor/skills/minihub-plugin-l3/` | `#if CONFIG_*` Host options inside the plugin |
| Programming guide RST/MD | `.cursor/skills/minihub-docs/` | Dual-write RST and old MD as two sources of truth |

Scaffold a plugin:

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native
```

Templates: `docs/module_sdk/templates/`.

IDE MCP for plugin authors (Cursor / VS Code, **not** the Hub on-device MCP):

```bash
python3 tools/minihub_plugin_mcp.py
```

Repo configs: `.cursor/mcp.json`, `.vscode/mcp.json`. Tools: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin`.

## Host constraints

- Product = `application/<name>/`. Default public Host: `application/minihub`.
- Shared Core: `components/minihub_*`, Hub adapters under `components/minihub_adapters/`, Store plugins under `components/minihub_plugins/`. Products include `cmake/minihub_bundle/minihub_import.cmake` then `minihub_register_kconfig()`; or one yml path `cmake/minihub_bundle`. Do not put ecosystem-specific rules in Core.
- Vendor adapters ship as Store `.mhub`, not Host `require`.
- ESP-IDF **~6.2** + target **esp32s31** (`idf.py --preview`). IDF 5.x has no S31 toolchain.
- Optional **on-device** MCP: `CONFIG_MINIHUB_MCP_SERVER=y` + `git submodule update --init components/esp-claw`. Unrelated to the plugin-author IDE MCP above.

## Do not read unless needed

- `application/minihub/managed_components/`, `application/minihub/build/`
- `application/minihub/components/esp-rainmaker/` (upstream)
- `docs/_build/`, `.venv-docs/`

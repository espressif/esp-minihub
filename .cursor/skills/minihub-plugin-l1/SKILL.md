---
name: minihub-plugin-l1
description: Create or edit MiniHub Store L1 packs (module.json inject into an existing Host adapter, no main.js / no .so). Use when the user wants a config pack, inject devices into mqtt/zigbee/modbus, or kind pack.
---

# MiniHub Store L1 (pack)

L1 is a **Store packaging tier**, not a Host layer. It is **config only**: `module.json` (+ optional `inject` / `requires`). No `main.js`, no `CMakeLists.txt`, no native `.so`.

## Where this fits

| Layer | What belongs there |
|-------|--------------------|
| Host Core | `minihub_point_t`, rules, web, device-widget catalog |
| Bundled Host adapters | Zigbee / BLE Mesh / Matter / MQTT / Modbus via Kconfig (`components/minihub_adapters/`) |
| Store (L1 / L2 / L3) | Third-party / ecosystem modules shipped as `.mhub` |

L1 only **injects into an existing Host adapter**. Do not invent a new radio stack here (that is Host or L3).

## Where sources live

| Kind | Location |
|------|----------|
| Real Store pack | `components/minihub_plugins/<id>/` (scaffold default) |
| Standalone demos | `examples/plugins/<id>/` + `MINIHUB_PLUGIN_DIRS` — **not** Host `require`, **not** default `store_preinstall`. Reference: `examples/plugins/ble_mesh_pack` |

Public `application/minihub` keeps `store_preinstall: []`. Do not bake demo packs into Host builtin.

## Scaffold

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
```

Or IDE MCP: `scaffold_plugin` with `tier=l1` (`python3 tools/minihub_plugin_mcp.py`).

## `module.json`

- `id` = directory name (must match folder)
- `kind`: `"pack"` (or omit if `runtime` is `"pack"` / `config_only: true`)
- `inject.adapter` must already exist on Host (`mqtt`, `zigbee`, `ble_mesh`, `modbus`, …)

## Env

| Variable | Default | Meaning |
|----------|---------|---------|
| `MINIHUB_APP` | `minihub` | Product under `application/` |
| `MINIHUB_PLUGINS_DEPLOY_DIR` | `~/workspace/minihub_store` | Catalog + `.mhub` |
| `MINIHUB_PLUGIN_DIRS` | (empty) | Extra source trees (`:`/`;`). Needed only if the pack is **not** under `components/minihub_plugins/` |

No firmware compile. Packer zips `module.json` only (no `main.js`, no `.so`).

## Pack (zip → `.mhub`)

```bash
# optional if sources are under examples/plugins/<id>/
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"

python3 tools/pack_store_module.py pack my_pack
python3 tools/sync_store_catalog.py all
# or: python3 tools/sync_store_catalog.py my_pack --deploy ~/workspace/minihub_store
python3 tools/store_server.py --port 8080
```

## Artifacts

```text
$MINIHUB_PLUGINS_DEPLOY_DIR/catalog.json
$MINIHUB_PLUGINS_DEPLOY_DIR/my_pack/my_pack.mhub   # zip: module.json
```

## Install

- **Store (usual):** Hub Store `custom_url` → catalog host → refresh → install. Lands on device as `/store/modules/<id>/`.
- **Factory preinstall (optional):** `application/<app>/manifest.yml` → `store_preinstall: [my_pack]`, then rebuild/flash. Public Host keeps `[]`. `module.json` `"preinstall"` is ignored for factory bundling.

Docs: `docs/en/plugins/develop_l1.rst` / `pack_and_catalog.rst`, `tools/README.md`.

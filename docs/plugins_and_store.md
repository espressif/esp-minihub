# Plugins & Store

## Host vs plugin

| In Host (static) | Via Store (`.mhub`) |
|------------------|---------------------|
| Core, Web, MCP gateway hooks | Ecosystem / vendor adapters |
| Zigbee / BLE Mesh / Matter / MQTT / Modbus adapters (Kconfig) | L1 pack / L2 JS / L3 native modules |

Do not add vendor cloud credentials or default device rules to Host `config/` for a public release.

## Module tiers

| Tier | Contents | Artifact |
|------|----------|----------|
| L1 | `module.json` (+ inject) | `<id>.mhub` zip |
| L2 | + `main.js` (+ `ui/` / `web/`) | `<id>.mhub` |
| L3 | + native `.so` (MHMD) | `<id>.mhub` (+ MHMD intermediate) |

Sources (optional): `components/minihub_plugins/<id>/`.  
Scaffold: `python3 tools/new_store_module.py --tier l1|l2|l3 --id <id>` (templates in `docs/module_sdk/templates/`).  
IDE MCP for authors: `python3 tools/minihub_plugin_mcp.py` (see `docs/module_sdk/README.md`).  
Deploy root: `MINIHUB_STORE_DEPLOY` (default `~/workspace/minihub_store`).

```bash
python3 tools/pack_store_module.py pack <id>|all          # outer .mhub (default --format zip)
python3 tools/pack_store_module.py mhmd <so> <out.so>     # inner MHMD header on native .so
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py
```

| Concern | Name | Notes |
|---------|------|--------|
| Outer package | `--format` / `MHUB_PACK_FORMAT` | Default `zip`. Not `MHMD_*` — MHMD is the native image magic. |
| Inner native | `mhmd` subcommand | MHMD + ELF; Host loads via elf_loader |

Host unpack today: `hub_zip.c` (zip only).

## Config

- Runtime adapters / rules / scenes: `application/<app>/config/{adapters,rules,scenes}.json` if present, else repo `config/`
- Factory Store modules: `application/<app>/manifest.yml` → `store_preinstall: [<id>, …]` (into `/store/modules/`); `module.json` `"preinstall"` is ignored for factory bundling. Catalog + `/store/files/<id>.mhub` still stage all `minihub_plugins/` for on-device install.
- Manifest / component require: `config/minihub_manifest.yml` (`_host_base`) + `application/<app>/manifest.yml` + `tools/sync_host_manifest.py`

## Native load guard

L3 native modules write `/store/modules/<id>/.load_guard` around `dlopen`/`entry`:

| Content | Meaning |
|---------|---------|
| `loading` | Load started; if still present after reboot → previous crash mid-load → **auto-skip** |
| `ok` | Last load finished |

Clear / retry:

```bash
# Clear sticky guard and reload
curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
  -d '{"id":"my_native","clear_load_guard":true,"reload":true}'
# Or enable=true (also clears the guard)
curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
  -d '{"id":"my_native","enabled":true,"reload":true}'
```

`GET /api/modules` reports `load_guard` and `load_blocked`.

## Native symbol allowlist

Plugins may leave Host symbols undefined; Host registers them at load time. Public snapshot: [module_sdk/host_export_allowlist.json](module_sdk/host_export_allowlist.json). Regenerate with `tools/gen_host_export_symbols.py` after rebuilding modules.

## L3 runtime compatibility

Host firmware and Store `.mhub` are **loosely coupled**. After a full firmware flash, plugins built with **different link flags** or **pack dates** can load as long as:

| Requirement | Notes |
|-------------|--------|
| **Same SoC target** | e.g. `esp32s31` — cross-chip `.so` will not relocate |
| **`abi_version`** | Must match `MINIHUB_MODULE_ABI_VERSION` in Host |
| **`core_api_min` / `core_api_max`** | Must include Host `MINIHUB_CORE_API_VERSION` |
| **Undefined symbols** | Subset of Host allowlist; add new deps → regenerate `host_elf_symbols_generated.c` |
| **No Host Kconfig in plugins** | Use `minihub_module_host_api_t` only; do not `#if CONFIG_*` Host options |

**Not required to match:** `CONFIG_ELF_FILE_SYSTEM_BASE_PATH`, feature Kconfig, or exact GNU ld page size — Host tolerates varied PT_LOAD layouts (PSRAM cache sync on S31 after `dlopen` / before `entry`).

**Recommended for new packages:** link with `-Wl,-z,max-page-size=4` (see `cmake/minihub_module_so.cmake`). `tools/check_module_so_layout.py` warns on large vaddr gaps but does not block packaging.

**Future ABI bumps:** increment `MINIHUB_MODULE_ABI_VERSION` and extend `minihub_module_host_api_t` at the tail; old plugins fail with a clear version error instead of crashing.

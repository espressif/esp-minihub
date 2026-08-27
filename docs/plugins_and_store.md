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

Sources (optional): `components/store_modules/<id>/` (e.g. `miot` L3 plugin).  
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
- Factory Store modules: `application/<app>/manifest.yml` → `store_preinstall: [<id>, …]` (into `/store/modules/`); `module.json` `"preinstall"` is ignored for factory bundling. Catalog + `/store/files/<id>.mhub` still stage all `store_modules/` for on-device install.
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
  -d '{"id":"miot","clear_load_guard":true,"reload":true}'
# Or enable=true (also clears the guard)
curl -X POST http://$HUB/api/modules -H 'Content-Type: application/json' \
  -d '{"id":"miot","enabled":true,"reload":true}'
```

`GET /api/modules` reports `load_guard` and `load_blocked`.

## Native symbol allowlist

Plugins may leave Host symbols undefined; Host registers them at load time. Public snapshot: [module_sdk/host_export_allowlist.json](module_sdk/host_export_allowlist.json). Regenerate with `tools/gen_host_export_symbols.py` after rebuilding modules.

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

- Runtime adapters: `config/adapters.json` (empty device lists in public tree)
- Rules / scenes: `config/rules.json`, `config/scenes.json`
- Manifest / component require: `config/minihub_manifest.yml` + `tools/sync_host_manifest.py`

## Native symbol allowlist

Plugins may leave Host symbols undefined; Host registers them at load time. Public snapshot: [module_sdk/host_export_allowlist.json](module_sdk/host_export_allowlist.json). Regenerate with `tools/gen_host_export_symbols.py` after rebuilding modules.

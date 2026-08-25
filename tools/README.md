# Tools

| Script | Role |
|--------|------|
| `build_firmware.py` | Build product (`-app` / `MINIHUB_APP`) / modules / merge |
| `apply_sdkconfig_variant.sh` | Copy `variants/<name>.yml` → `app.yml` |
| `compose_sdkconfig.py` | Compose sdkconfig defaults from `app.yml` + features |
| `pack_store_module.py` | `pack` → `.mhub`; `mhmd` → MHMD wrap |
| `build_store_modules.py` | Build + pack native Store modules |
| `sync_host_manifest.py` | Apply manifest for `MINIHUB_APP` |
| `sync_store_catalog.py` | Store `catalog.json` |
| `store_server.py` | Local Store HTTP |
| `gen_host_export_symbols.py` | Host ELF export allowlist |
| `apply_component_patches.sh` | Re-apply `patches/` |
| `ci/` | GitLab helpers |
| `ui_font/` | LCD font subset |
| `git-hooks/` | Local hooks |

**Product** = `application/<name>/` only (`minihub`, `claw_minihub`).

See [docs/build.md](../docs/build.md).

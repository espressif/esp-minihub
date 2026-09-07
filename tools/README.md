# Tools

| Script | Role |
|--------|------|
| `build_firmware.py` | Build product (`-app` / `MINIHUB_APP`) / modules / merge |
| `apply_sdkconfig_variant.sh` | Copy `variants/<name>.yml` → `app.yml` |
| `compose_sdkconfig.py` | Compose sdkconfig defaults from `app.yml` + features |
| `new_store_module.py` | Scaffold L1/L2/L3 from `docs/module_sdk/templates/` |
| `pack_store_module.py` | `pack` → `.mhub`; `mhmd` → MHMD wrap |
| `build_store_modules.py` | Build + pack native Store modules |
| `product_assets.py` | Resolve app/repo config JSON; list `store_preinstall` |
| `sync_host_manifest.py` | Merge `_host_base` + `application/<app>/manifest.yml` for `MINIHUB_APP` |
| `sync_store_catalog.py` | Store `catalog.json` |
| `store_server.py` | Local Store HTTP (+ `/api/miot-spec/*`) |
| `gen_host_export_symbols.py` | Host ELF export allowlist |
| `apply_component_patches.sh` | Re-apply `patches/` |
| `ci/` | GitLab helpers (`install_git_hooks.sh` → local pre-commit + pre-push) |
| `ui_font/` | LCD font subset |
| `git-hooks/` | Local hooks |

**Product** = `application/<name>/` only (`minihub`).

See [docs/build.md](../docs/build.md).

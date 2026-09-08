# Tools

Product = `application/<name>/` only (default `minihub`). Longer context: [docs/build.md](../docs/build.md), [docs/plugins_and_store.md](../docs/plugins_and_store.md), [docs/module_sdk/README.md](../docs/module_sdk/README.md).

Env used by several scripts:

| Variable | Default | Meaning |
|----------|---------|---------|
| `MINIHUB_APP` | `minihub` | Product folder under `application/` |
| `MINIHUB_STORE_DEPLOY` | `~/workspace/minihub_store` | Store catalog + `<id>/<id>.mhub` |

---

## Firmware

### `build_firmware.py`

Unified IDF build for a product. Passes remaining args to `idf.py`.

```bash
./tools/build_firmware.py [-app NAME] [stacks...] [--] [idf.py args...]
./tools/build_firmware.py modules [all|module_id ...]   # pack native Store modules
./tools/build_firmware.py merge                         # merge flash image
```

Stacks (lowercase): `ble_mesh`, `zigbee`, `matter`, `thread`. `thread` and `zigbee` are mutually exclusive. Default stacks if omitted: `zigbee ble_mesh`.

```bash
./tools/build_firmware.py -app minihub zigbee ble_mesh build
./tools/build_firmware.py thread matter build
./tools/build_firmware.py build merge
```

### `apply_sdkconfig_variant.sh`

Copy `application/<app>/config/sdkconfig/variants/<name>.yml` over `app.yml`.

```bash
./tools/apply_sdkconfig_variant.sh lcd
./tools/apply_sdkconfig_variant.sh --restore    # variants/default.yml
```

### `compose_sdkconfig.py`

Library used by the firmware build: expand `app.yml` + feature fragments into `SDKCONFIG_DEFAULTS`. Not a daily CLI.

### `sync_host_manifest.py`

Merge `config/minihub_manifest.yml` (`_host_base`) with `application/<app>/manifest.yml` → component require / adapters setup.

```bash
python3 tools/sync_host_manifest.py --app minihub
```

### `product_assets.py`

Resolve product config JSON (`application/<app>/config/` then repo `config/`) and factory Store ids from `manifest.yml` → `store_preinstall`.

```bash
python3 tools/product_assets.py --app minihub resolve adapters.json
python3 tools/product_assets.py --app minihub store-preinstall
python3 tools/product_assets.py --app minihub seed-config /tmp/hub_config
```

### `apply_component_patches.sh`

Re-apply `patches/` after managed-component re-download. Optional `Apply-Root:` in the patch header.

```bash
./tools/apply_component_patches.sh
./tools/apply_component_patches.sh --check    # dry-run; exit 1 if a patch is needed
```

---

## Store plugins

Ids: lowercase + underscores, no hyphens (`^[a-z][a-z0-9_]{0,30}$`).

### `new_store_module.py`

Copy `docs/module_sdk/templates/l{1,2,3}/` → `components/store_modules/<id>/`.

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native --force
```

### `pack_store_module.py`

Outer `.mhub` is zip. `mhmd` wraps a linked `.so` with the MHMD header (not a zip format).

```bash
python3 tools/pack_store_module.py pack my_js
python3 tools/pack_store_module.py pack all --deploy ~/workspace/minihub_store
python3 tools/pack_store_module.py mhmd build/foo.so build/foo_mhmd.so
```

Host unpacks zip only (`hub_zip.c`). `--format` / `MHUB_PACK_FORMAT` reserved; only `zip` is implemented.

### `build_store_modules.py`

After a Host build: wrap `application/<app>/build/modules/libadapter_<id>.so` → MHMD + `.mhub`.

```bash
python3 tools/build_store_modules.py all
python3 tools/build_store_modules.py my_native --deploy ~/workspace/minihub_store
```

Also reachable as `./tools/build_firmware.py modules`.

### `sync_store_catalog.py`

Pack module(s) and rewrite deploy `catalog.json`.

```bash
python3 tools/sync_store_catalog.py all
python3 tools/sync_store_catalog.py my_js --deploy ~/workspace/minihub_store
```

### `store_server.py`

LAN Store HTTP (default `:8080`): `catalog.json` and `/files/<id>.mhub`.

```bash
python3 tools/store_server.py --port 8080
python3 tools/store_server.py --deploy ~/workspace/minihub_store --rebuild-slim
```

Point Hub Store at this host (`custom_url`), then refresh + install.

### `minihub_plugin_mcp.py`

stdio **IDE MCP** for plugin authors (Cursor / VS Code). Not `CONFIG_MINIHUB_MCP_SERVER`.

```bash
python3 tools/minihub_plugin_mcp.py
python3 tools/plugin_mcp/test_plugin_mcp.py
```

Repo wiring: `.cursor/mcp.json`, `.vscode/mcp.json`. Tools: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin`. Implementation: `tools/plugin_mcp/`.

### `store_paths.py`

Shared path helpers (`STORE_MODULES`, deploy dir). Import only.

### `check_module_so_layout.py`

Fail if a `libadapter_*.so` PT_LOAD vaddr gap is too large for S31 elf_loader. Link with `-Wl,-z,max-page-size=4`.

```bash
python3 tools/check_module_so_layout.py application/minihub/build/modules/libadapter_my_native.so
python3 tools/check_module_so_layout.py path/to.so --warn-only
```

### `gen_host_export_symbols.py`

Scan adapter `.so` undefined symbols → Host export `.c` and `docs/module_sdk/host_export_allowlist.json`. Run after a successful module link if you add libc/IDF symbols.

```bash
python3 tools/gen_host_export_symbols.py \
  --modules-dir application/minihub/build/modules \
  --out components/minihub_module/host_elf_symbols_generated.c
```

---

## CI and git

| Path | Role |
|------|------|
| `ci/list_apps.py` | Print `application/*` names that have `CMakeLists.txt` |
| `ci/build_apps.sh` | Build (+ merge) each app; does **not** pack `.mhub` |
| `ci/pack_store_modules.sh` | Pack Store after a minihub firmware build |
| `ci/stage_artifacts.sh <app>` | Copy firmware / flash images / sdkconfig into `ci_artifacts/` |
| `ci/install_git_hooks.sh` | Install pre-commit + pre-push (+ strip Cursor commit trailers) |
| `git-hooks/commit-msg-strip-cursor` | Local commit-msg helper |

```bash
bash tools/ci/install_git_hooks.sh
MINIHUB_APP=minihub bash tools/ci/build_apps.sh
```

---

## LCD font (`ui_font/`)

Subset Source Han Sans for `components/minihub_lcd/ui/fonts/minihub_sc.otf`. Needs `pyftsubset` and the LVGL bundled OTF (or `SRC_FONT`).

```bash
./tools/ui_font/build_subset.sh
MINIHUB_FONT_UI_ONLY=1 ./tools/ui_font/build_subset.sh   # smaller, UI ASCII only
```

`collect_chars.py` / `gen_chars_3000.py` feed the subset list; you usually run the shell wrapper.

Vendor-only helpers (not this directory): `components/store_modules/<id>/tools/`.

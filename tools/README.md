# Tools

[English](README.md) | [中文](README_CN.md)

Product = `application/<name>/` only (default `minihub`). Longer context: [docs/en/getting_started.rst](../docs/en/getting_started.rst), [docs/en/plugins/](../docs/en/plugins/index.rst), [docs/module_sdk/README.md](../docs/module_sdk/README.md).

Env used by several scripts:

| Variable | Default | Meaning |
|----------|---------|---------|
| `MINIHUB_APP` | `minihub` | Product folder under `application/` |
| `MINIHUB_PLUGINS_DEPLOY_DIR` | `~/workspace/minihub_store` | Deploy root: `Stores/Common/<id>/`, `Firmwares/`, `Assets/`, `Webs/` |

---

## Dev sync panel (local visual push)

### `dev_sync.py` + `dev_sync.html`

Browser panel to upload/download to the device: **Web** (html/css/js), **adapters.json**, and **type-catalog CSV** (`devices.csv` / `parameters.csv`). Device REST has no CORS, so a local proxy serves the UI and forwards `/hub/*` → the device. Do not open the HTML via `file://`.

```bash
python3 tools/dev_sync.py --open          # http://127.0.0.1:8765/
python3 tools/dev_sync.py --port 9000
# Optional defaults: MINIHUB_DEV_BASE=http://192.168.1.50 MINIHUB_DEV_TOKEN=...
```

Enter the device URL (e.g. `http://minihub.local`) and optional Bearer. Needs firmware `CONFIG_MINIHUB_WEB_UI`; auth matches console REST.

| Resource | Device API | Device path |
|----------|------------|-------------|
| Web | `PUT /api/dev/web/<rel>`; read `GET /web/<rel>` | `/data/web/` |
| adapters | `GET/PUT /api/config/adapters` | `/data/config/adapters.json` |
| Type catalog | `GET/PUT /api/dev/catalog/{devices,parameters}.csv` | `/data/config/model/` → flat `/data/config/devices.csv` + reload |
| Icons | (panel notes: coming soon) | `/data/images/…` |

Type catalog docs: [`components/minihub_device_widget/README.md`](../components/minihub_device_widget/README.md#remote-catalog).

---

## Firmware

### `build_firmware.py`

Unified IDF build for a product. Passes remaining args to `idf.py`.

```bash
./tools/build_firmware.py [-app NAME] [stacks...] [--] [idf.py args...]
./tools/build_firmware.py module [all|<id> ...]    # compile native module .so
./tools/build_firmware.py modules                  # compile every native .so
./tools/build_firmware.py pack [all|<id> ...]      # pack .mhub, no compile
./tools/build_firmware.py merge                    # merge flash image
```

Stacks (optional): `ble_mesh`, `zigbee`, `matter`, `thread`. `thread` and `zigbee` are mutually exclusive. If omitted, defaults are `zigbee ble_mesh`, then the script reconciles with the product `sdkconfig` (drops stacks that are disabled there; adds `matter` / `thread` when those options are already `=y`). Pass a stack name only when you want to force-enable it (and it is not explicitly disabled in `sdkconfig`).

```bash
./tools/build_firmware.py -app minihub build
./tools/build_firmware.py build merge
./tools/build_firmware.py module my_native
./tools/build_firmware.py modules
./tools/build_firmware.py pack my_native
```

`build` compiles the Host only. It does not compile native plugins. `modules` or `module <id>` compiles a native `.so` into the build tree and does not create a store plugin folder. `pack <id>` on a native module wraps the local MHMD `.so` and does not write `Stores/Common` or `Stores/<chip>`.

### `apply_sdkconfig_variant.sh`

Copy `application/<app>/configs/sdkconfig/variants/<name>.yml` over `app.yml`.

```bash
./tools/apply_sdkconfig_variant.sh --restore    # variants/default.yml
# LCD is -app minihub_lcd, not a minihub variant.
```

### `compose_sdkconfig.py`

Library used by the firmware build: expand `app.yml` + feature fragments into `SDKCONFIG_DEFAULTS`. Not a daily CLI.

### `sync_host_manifest.py`

Merge `configs/minihub_manifest.yml` (`_host_base`) with `application/<app>/manifest.yml` → component require / adapters setup.

```bash
python3 tools/sync_host_manifest.py --app minihub
```

### `product_assets.py`

Resolve product config JSON (`application/<app>/configs/` then repo `configs/`) and factory Store ids from `manifest.yml` → `store_preinstall`.

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

Copy `docs/module_sdk/templates/l{1,2,3}/` → `examples/plugins/<id>/`.

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native --force
```

### `build_plugin_standalone.py`

No-IDF build for an `examples/plugins/<id>` L3 module (CMake + Plugin SDK).
There is no in-tree L3 demo, so scaffold one first.

```bash
python3 tools/build_plugin_standalone.py my_native
python3 tools/build_plugin_standalone.py my_native --clean --pack
```

Requires `riscv32-esp-elf-gcc` (or `--cc` / `MINIHUB_PLUGIN_CC`) and `cmake` on `PATH`.
CMake tree + MHMD: `examples/plugins/<id>/build/<id>.so` (`build/` is gitignored).
Host in-tree MHMD stays at `application/$MINIHUB_APP/build/<id>.so`. Shared logic:
`cmake/minihub_plugin_standalone.cmake` (also documented for third-party copy-paste).

### `pack_store_module.py`

Outer `.mhub` is zip. `mhmd` wraps a linked `.so` with the MHMD header (not a zip format).

```bash
python3 tools/pack_store_module.py pack my_js
python3 tools/pack_store_module.py pack all --deploy ~/workspace/minihub_store
python3 tools/pack_store_module.py mhmd build/foo.so build/foo_mhmd.so
```

Host unpacks zip only (`hub_zip.c`). `--format` / `MHUB_PACK_FORMAT` reserved; only `zip` is implemented.
`pack all` skips native and hybrid modules. A named native id wraps a local MHMD `.so` and is not written to the store.

### `build_store_modules.py`

After a Host build: wrap `application/<app>/build/modules/libadapter_<id>.so` → MHMD + `.mhub`.

```bash
python3 tools/build_store_modules.py all
python3 tools/build_store_modules.py my_native --deploy ~/workspace/minihub_store
```

Also: `./tools/build_firmware.py pack`.

### `sync_store_catalog.py`

Rewrite `Stores/Common/<id>/catalog.json` for L1 and L2. A native package found in the store is removed and is not moved under `Common` or a chip folder. No derived array is written to the deploy tree (`/api/catalog` aggregates per-plugin catalogs at request time); pass `--aggregate PATH` to emit one array at an explicit path. `targets` is only the chip filter. Default (no args): walk those `.mhub` files only — no sources, no `MINIHUB_PLUGIN_DIRS`. `--from-store` is the same. Pass `all` or an id to pack from sources first, then rewrite the catalogs the same way. The script is also copied into `module_sdk.zip`.

```bash
python3 tools/sync_store_catalog.py
python3 tools/sync_store_catalog.py --deploy ~/workspace/minihub_store
python3 tools/sync_store_catalog.py all
python3 tools/sync_store_catalog.py my_js --deploy ~/workspace/minihub_store
```

### `store_server.py`

LAN deploy HTTP (default `:8080`): `GET /api/catalog`, `GET /api/firmware`, and `GET /files/<path>`.

```bash
python3 tools/store_server.py --port 8080
python3 tools/store_server.py --deploy ~/workspace/minihub_store
```

Point Hub Store at this host (`custom_url`), then refresh + install.

### `minihub_plugin_mcp.py`

stdio **IDE MCP** for plugin authors (Cursor / VS Code). Not `CONFIG_MINIHUB_MCP_SERVER`.

```bash
python3 tools/minihub_plugin_mcp.py
python3 tools/plugin_mcp/test_plugin_mcp.py
```

Repo wiring: `.agent/mcp/cursor.mcp.json` and `.agent/mcp/vscode.mcp.json`, symlinked to `.cursor/mcp.json` and `.vscode/mcp.json`. Tools: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin`. Implementation: `tools/plugin_mcp/`.

### `store_paths.py`

Shared path helpers (`DEMO_PLUGINS`, deploy dir). Import only.

### `check_module_so_layout.py`

Checks `libadapter_*.so` PT_LOAD layout for elf_loader. Link with
`-Wl,-z,max-page-size=4`.

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

### `check_plugin_sdk.py`

Validate `docs/module_sdk/include/minihub_plugin_sdk.h` against the export allowlist and Host ABI.

```bash
python3 tools/check_plugin_sdk.py
```

### `preview_docs.py`

Serve `docs/_build/` HTML locally (default `127.0.0.1:8765`), at
`/<lang>/<version>/<page>.html` — the same URL shape the docs host publishes —
plus the flat `/<lang>/<page>.html` form. `/` redirects to the default language.

```bash
python3 tools/preview_docs.py
python3 tools/preview_docs.py --docs-version preview-my_branch --lang en
```

---

## CI and git

| Path | Role |
|------|------|
| `ci/install_git_hooks.sh` | Install pre-commit + pre-push (+ strip Cursor commit trailers) |
| `git-hooks/commit-msg-strip-cursor` | Local commit-msg helper |

Firmware / pack / docs CI jobs call `./tools/build_firmware.py` and `./tools/build_docs.sh` from `.gitlab-ci.yml` (no extra wrappers).

```bash
bash tools/ci/install_git_hooks.sh
./tools/build_firmware.py --app minihub build merge
```

---

## LCD screen UI (`lcd_ui_editor/`)

Tree editor for the device screen UI: the Brookesia JSON assets under `components/minihub_lcd/ui/` (screens / templates / flows). Stdlib only, loopback only.

```bash
python3 tools/lcd_ui_editor/server.py            # serve + open a browser
python3 tools/lcd_ui_editor/server.py --port 8791 --no-open
```

A save is **transactional**:

1. Back up.
2. Write.
3. Run the existing LCD UI contract suite (`tools/test_lcd_ui_contract.py`).
4. **Roll back and reject the save if the suite fails.**

The contract rules stay single-sourced, and a broken UI cannot land on disk. The writer keeps each file's own layout (the assets mix expanded / `{"a": 1}` / `{ "a": 1 }`), and a file whose content did not change is not rewritten at all.

`ui_contract.json` is **not** editable here — it is the C++-side name registry, not the visual tree. Details: [`lcd_ui_editor/README.md`](lcd_ui_editor/README.md).

---

## LCD font (`ui_font/`)

Subset Source Han Sans for `components/minihub_lcd/ui/fonts/minihub_sc.otf`
(UI strings ∪ full GB2312 level-1, 3755 Hanzi). Needs `pyftsubset` and the LVGL
bundled OTF (or `SRC_FONT`).

```bash
./tools/ui_font/build_subset.sh
MINIHUB_FONT_UI_ONLY=1 ./tools/ui_font/build_subset.sh   # smaller, UI ASCII only
```

`collect_chars.py` / `gen_gb2312_l1.py` feed the subset list; you usually run the shell wrapper.

## LCD device icons

32×32 PNGs under `components/minihub_lcd/ui/images/icon_*.png` (Brookesia
`device_icons` imageSet). Regenerated with purpose-drawn glyphs — do not
downscale the Web SVGs; at 32px they become unreadable:

```bash
python3 tools/regen_lcd_device_icons.py   # needs Pillow
```

Rebuild/flash **storage** after changing icons.

Vendor-only helpers (not this directory): plugin `tools/` under `MINIHUB_PLUGIN_DIRS`.

---
name: minihub-host
description: Develop MiniHub Host firmware products (application folder, Kconfig, JSON seeds, radio stacks, OEM copy). Use when the user changes Host, app_main, partitions, sdkconfig features, adapters.json, or copies a new product from minihub.
---

# MiniHub Host

## Read first

- `AGENTS.md`
- `docs/en/getting_started.rst` + `docs/en/host_customization.rst`

## Product layout

- Product = `application/<app>/` (`-app` / `MINIHUB_APP`).
- Shared Core = `components/minihub_*` and bundled adapters in `components/minihub_adapters/`. Product import: `cmake/minihub_bundle/minihub_import.cmake` + `minihub_register_kconfig()`. One-yml: `path: …/cmake/minihub_bundle`.
- Seeds: `application/<app>/config/{adapters,rules,scenes}.json` else repo `config/`.
- Manifest: `application/<app>/manifest.yml` extends `config/minihub_manifest.yml` `_host_base`.
- Device UI catalog: `components/minihub_device_widget/model/*.csv` (closed widgets: toggle / slider / dropdown / text / hidden) → runtime `/data/config/devices.csv`. Core stays type-agnostic (`minihub_point_t`; one device = one point, params in `props.params`).

## Host vs Store (do not confuse)

| In Host | Via Store `.mhub` |
|---------|-------------------|
| Core, Web, device-widget catalog | L1 pack / L2 JS / L3 native modules |
| Bundled adapters (Zigbee / BLE Mesh / Matter / MQTT / Modbus, …) | Vendor / demo plugins |

- Do **not** add vendor cloud adapters or demo plugins to Host `require`.
- Public `application/minihub` uses `store_preinstall: []`. Optional factory ids resolve under `examples/plugins/<id>/` **or** `MINIHUB_PLUGIN_DIRS`.
- Standalone demos live in `examples/plugins/` — not Host builtin.

## Plugins vs Host build

| Goal | What to do |
|------|------------|
| Pull extra sources into configure | `export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"` **before** `./tools/build_firmware.py … build` (`:`/`;` separated). Read at configure time only. |
| Ship `.mhub` to LAN Store | After L3 Host build (or L1/L2 pack): `$MINIHUB_PLUGINS_DEPLOY_DIR` (default `~/workspace/minihub_store`) has `catalog.json` + `<id>/<id>.mhub`. Serve with `python3 tools/store_server.py`. |
| Rebuild all L3 `.so` | `./tools/build_firmware.py modules` |
| Rebuild one L3 `.so` | `./tools/build_firmware.py module <id>` |
| Re-wrap existing L3 ELF | `./tools/build_firmware.py pack [all\|<id>]` |
| Factory-flash a module | App `manifest.yml` `store_preinstall: [<id>]` then rebuild/flash. Does not replace Store install. Public product stays `[]`. |

L1/L2: no Host link step — `python3 tools/pack_store_module.py pack <id>` then `sync_store_catalog.py`. L3: `./tools/build_firmware.py module <id>` produces `application/<app>/build/modules/libadapter_<id>.so`; `./tools/build_firmware.py pack <id>` wraps MHMD `build/<id>.so` into `.mhub`. Details: `.cursor/skills/minihub-plugin-l{1,2,3}/`.

## Workflow

1. Copy `application/minihub` → `application/<new>` only if creating a new product.
2. Edit **app** `manifest.yml` / `config/sdkconfig/app.yml` / JSON seeds. Touch `_host_base` only for all products.
3. `MINIHUB_APP=<app> python3 tools/sync_host_manifest.py`
4. Build (IDF ~6.2, `esp32s31`):

```bash
export IDF_PATH=/path/to/esp-idf-v6.2
# source "$IDF_PATH/export.sh"
./tools/build_firmware.py -app minihub zigbee ble_mesh build
```

`thread` and `zigbee` cannot be combined. Matter needs `ESP_MATTER_PATH`.

## MCP

`CONFIG_MINIHUB_MCP_SERVER` is default n. Enable only with `git submodule update --init components/esp-claw`.

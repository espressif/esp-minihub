---
name: minihub-host
description: Develop MiniHub Host firmware products (application folder, Kconfig, JSON seeds, radio stacks, OEM copy). Use when the user changes Host, app_main, partitions, sdkconfig features, adapters.json, or copies a new product from minihub.
---

# MiniHub Host

## Read first

- `AGENTS.md`
- `docs/build.md` and, if present, `docs/en/getting_started.rst` + `docs/en/host_customization.rst`

## Product layout

- Product = `application/<app>/` (`-app` / `MINIHUB_APP`).
- Shared Core = `components/minihub_*` and `components/minihub_adapters/`. Product import: `cmake/minihub_bundle/minihub_import.cmake` + `minihub_register_kconfig()`. One-yml: `path: …/cmake/minihub_bundle`.
- Seeds: `application/<app>/config/{adapters,rules,scenes}.json` else repo `config/`.
- Manifest: `application/<app>/manifest.yml` extends `config/minihub_manifest.yml` `_host_base`.

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

5. Do **not** add vendor cloud adapters to Host `require`. Use Store plugins + optional `store_preinstall`.

## MCP

`CONFIG_MINIHUB_MCP_SERVER` is default n. Enable only with `git submodule update --init components/esp-claw`.

---
name: minihub-plugin-l3
description: Create or edit MiniHub Store L3 native plugins (MHMD .so, minihub_module_entry, adapter vtable). Use when the user wants a C/C++ Store module, ELF plugin, or host_export_allowlist changes.
---

# MiniHub Store L3 (native)

L3 is a **Store packaging tier**: native MHMD `.so` loaded at runtime (not statically linked into Host by default). Reference demos: `examples/plugins/color_bulb`, `examples/plugins/meter_plug`.

## Where this fits

| Layer | What belongs there |
|-------|--------------------|
| Host Core | Generic `minihub_point_t` only — **no** per-type C structs (`LightEntity`, …) |
| Bundled Host adapters | Built-in stacks under `components/minihub_protocol_adapters/` (Kconfig) |
| Store L3 | Third-party / demo native adapters as `.so` + `.mhub` |

## Where sources live

| Kind | Location |
|------|----------|
| Production / vendor plugin | sibling repo `esp-minihub-plugins` via `MINIHUB_PLUGIN_DIRS` |
| Standalone demos | `examples/plugins/<id>/` via `export MINIHUB_PLUGIN_DIRS=...` at **configure** time |

Do **not** move demos into Host `require` or leave them in public `store_preinstall` (default `[]`). Optional temporary preinstall: app `manifest.yml` + rebuild; clear afterward.

## Scaffold

```bash
python3 tools/new_store_module.py --tier l3 --id my_native --name "My native"
```

IDE MCP can scaffold L3 files but **cannot** compile `.so`. Prefer copying patterns from `examples/plugins/color_bulb` over inventing entity structs.

## Device instance pattern

- **One device = one point** (`<protocol>.<device>`, e.g. `sim.color_bulb`).
- Live params in `props.params.<name> = { value, optional access / visible / min / max / step / unit }`.
- Instance list: plugin `devices.json` → generate `device_desc.gen.h` (`tools/generate_devices_property_from_json.py`).
- Type UI catalog is **Host-side**: `components/minihub_device_widget/model/*.csv` → embed → runtime `/data/config/devices.csv`. Closed widgets: `toggle` / `slider` / `dropdown` / `text` / `hidden`.
- Plugins reuse catalog types (`light`, `switch`, …); do **not** ship labels / widget CSS / icons inside the plugin.

## Must

- Export `minihub_module_entry(host, config, out_adapter)` (`MINIHUB_MODULE_ENTRY_SYMBOL`)
- Optional `minihub_module_rpc`
- Call Core only via `minihub_module_host_api_t` (honor `struct_size`)
- `REGISTER_ADAPTER` only inside `#if !CONFIG_MINIHUB_DYNAMIC_MODULES`
- `abi_version` / `core_api_min` / `core_api_max` match Host (`minihub_module_abi.h`)
- Undefined symbols ⊆ `docs/module_sdk/host_export_allowlist.json`
- Same SoC as Host (`esp32s31`); link `-Wl,-z,max-page-size=4` (see `cmake/minihub_module_so.cmake`)
- No `#if CONFIG_*` for Host feature flags inside the plugin

## Env

| Variable | Default | Meaning |
|----------|---------|---------|
| `MINIHUB_APP` | `minihub` | Product / `application/<app>/build` |
| `MINIHUB_PLUGINS_DEPLOY_DIR` | `~/workspace/minihub_store` | `Stores/ESP32S31/<id>/` when `targets` is that chip; `Stores/Common/<id>/` only for `["ALL"]` |
| `MINIHUB_PLUGIN_DIRS` | (empty) | Extra plugin trees (`:`/`;`). **Configure-time** (`cmake/minihub_bundle/minihub_import.cmake`). Does **not** auto-preinstall |

Needs Host `CONFIG_MINIHUB_DYNAMIC_MODULES` (product feature `dynamic_modules`). Link flags: `cmake/minihub_module_so.cmake` (`-Wl,-z,max-page-size=4`).

## Compile + pack

`new_store_module.py` only copies sources — it does **not** produce `.so`. IDE MCP cannot compile either.

If the plugin has `devices.json`:

```bash
python3 tools/generate_devices_property_from_json.py \
    examples/plugins/color_bulb/devices.json \
    examples/plugins/color_bulb/device_desc.gen.h
```

Production / vendor plugin (set `MINIHUB_PLUGIN_DIRS`):

```bash
./tools/build_firmware.py -app minihub zigbee ble_mesh build
# cmake also wraps MHMD + packs .mhub + refreshes catalog
```

Demo under `examples/plugins/` — export **before** configure/build:

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # or .../examples/plugins/color_bulb
./tools/build_firmware.py -app minihub build
```

Rebuild one native module, then pack the existing `.so`:

```bash
./tools/build_firmware.py module my_native
./tools/build_firmware.py pack my_native
# equivalent pieces:
python3 tools/pack_store_module.py mhmd \
    application/minihub/build/modules/libadapter_my_native.so \
    application/minihub/build/my_native.so
python3 tools/pack_store_module.py pack my_native
python3 tools/sync_store_catalog.py
python3 tools/store_server.py --port 8080
```

`pack` without a prior MHMD `.so` fails (`missing application/<app>/build/<id>.so`).

## Artifacts

| Path | What |
|------|------|
| `application/<app>/build/modules/libadapter_<id>.so` | Linked ELF (stripped) |
| `application/<app>/build/<id>.so` | MHMD header + ELF (local only; not left in deploy) |
| `$MINIHUB_PLUGINS_DEPLOY_DIR/Stores/Common/catalog.json` | Derived catalog array (every bucket) staged into the modstore image |
| `$MINIHUB_PLUGINS_DEPLOY_DIR/Stores/ESP32S31/<id>/<id>.mhub` | Zip for an ESP32S31 target: `module.json` + `<id>.so` [+ ui/web] |
| `application/<app>/build/modstore_image/files/<id>.mhub` | Copy used for on-device Store littlefs image |

## Install

- **Store (usual, no manifest change):** Hub Store `custom_url` → refresh → install. Store / `module_attach` creates the `adapters.json` slot (`ensure_adapter_slot`); without it, `load_all` skips the adapter.
- **Factory preinstall (optional):** `store_preinstall: [<id>]` in app `manifest.yml`, then rebuild/flash **with** the same `MINIHUB_PLUGIN_DIRS` if the source is a demo. Public Host keeps `[]`. Clear the list when done.

Load guard: `/store/modules/<id>/.load_guard`. Clear via `POST /api/modules` with `clear_load_guard` / `enabled`.

If you add a new libc/IDF symbol, regenerate allowlist with `tools/gen_host_export_symbols.py` after a successful module link.

Docs: `docs/en/plugins/`, `tools/README.md`, `examples/plugins/*/README.md`.

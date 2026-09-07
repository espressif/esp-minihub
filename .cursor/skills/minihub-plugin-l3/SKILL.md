---
name: minihub-plugin-l3
description: Create or edit MiniHub Store L3 native plugins (MHMD .so, minihub_module_entry, adapter vtable). Use when the user wants a C/C++ Store module, ELF plugin, or host_export_allowlist changes.
---

# MiniHub Store L3 (native)

## Scaffold

```bash
python3 tools/new_store_module.py --tier l3 --id my_native --name "My native"
```

Start from the L3 template, not an existing vendor adapter.

## Must

- Export `minihub_module_entry(host, config, out_adapter)` (`MINIHUB_MODULE_ENTRY_SYMBOL`)
- Optional `minihub_module_rpc`
- Call Core only via `minihub_module_host_api_t` (honor `struct_size`)
- `REGISTER_ADAPTER` only inside `#if !CONFIG_MINIHUB_DYNAMIC_MODULES`
- `abi_version` / `core_api_min` / `core_api_max` match Host (`minihub_module_abi.h`)
- Undefined symbols ⊆ `docs/module_sdk/host_export_allowlist.json`
- Same SoC as Host (`esp32s31`); link `-Wl,-z,max-page-size=4` (see `cmake/minihub_module_so.cmake`)
- No `#if CONFIG_*` for Host feature flags inside the plugin

## Build / pack

```bash
./tools/build_firmware.py -app minihub zigbee ble_mesh build
python3 tools/pack_store_module.py pack my_native
python3 tools/sync_store_catalog.py all
```

Load guard: sticky `/store/modules/<id>/.load_guard`. Clear via `POST /api/modules` with `clear_load_guard` / `enabled`.

If you add a new libc/IDF symbol, regenerate allowlist with `tools/gen_host_export_symbols.py` after a successful module link.

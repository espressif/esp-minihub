# MiniHub examples

[English](README.md) | [中文](README_CN.md)

## plugins/

| Directory | Tier | Purpose |
|-----------|------|---------|
| [ble_mesh_pack](plugins/ble_mesh_pack/) | **L1** | Merge extra BLE Mesh `provision.bind_models` into Host `adapters.ble_mesh`. No points, no JS, no `.so`. |
| [color_bulb_js](plugins/color_bulb_js/) | **L2** | Three simulated `type=light` points (`sim.js_kitchen`, `sim.js_desk`, `sim.js_bedroom`) + JS command handler. Devices-page widgets come from the Host type catalog. |
| [meter_plug_js](plugins/meter_plug_js/) | **L2** | One simulated metered switch (`sim.plug`: power + V/A/W, metering read-only). |

No L3 demo ships in-tree: a native `.so` must be built against one Host ABI, so
the guide uses `tools/new_store_module.py --tier l3` scaffolding instead (see
`docs/en/plugins/develop_l3.rst`).

Vendor Store plugins are not in this tree. Point `MINIHUB_PLUGIN_DIRS` at a sibling plugin repo before configure / pack.

Radio OTA plugins **must not** `point_register` a second id. Enrich the Host
point with `merge_props` (`owner` / `ota`).

## Pack (development PC)

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack <id>    # or: pack all (skips native)
python3 tools/sync_store_catalog.py
python3 tools/store_server.py --port 8080
```

On the Hub: Store `custom_url` → refresh → install. REST `type` is `pack` for
L1, `module` for L2/L3. Then `POST /api/hub/reload`.

L3 also needs a matching `.so`. In-tree: export `MINIHUB_PLUGIN_DIRS` **before**
`./tools/build_firmware.py modules`. A Host `build` does not compile native
plugins. Without an IDF project (needs
`riscv32-esp-elf-gcc` and `cmake`; MHMD file is `plugins/<id>/build/<id>.so`):

```bash
python3 tools/new_store_module.py --tier l3 --id my_native
python3 tools/build_plugin_standalone.py my_native
```

Then pack as above. L1/L2 zip only — no firmware compile. L3 needs a
`.so` (`build_plugin_standalone.py` or `./tools/build_firmware.py modules`).

Scaffold for a new module: `python3 tools/new_store_module.py --tier l1|l2|l3 --id …`
(default destination is `examples/plugins/`; vendor trees use `--out` / `MINIHUB_PLUGIN_DIRS`).

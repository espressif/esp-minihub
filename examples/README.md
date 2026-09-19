# MiniHub examples

[English](README.md) | [中文](README_CN.md)

## plugins/

| Directory | Tier | Purpose |
|-----------|------|---------|
| [ble_mesh_pack](plugins/ble_mesh_pack/) | **L1** | Merge extra BLE Mesh `provision.bind_models` into Host `adapters.ble_mesh`. No points, no JS, no `.so`. |
| [color_bulb_js](plugins/color_bulb_js/) | **L2** | Three simulated `type=light` points + JS command handler. Companion to L3 `color_bulb`. |
| [color_bulb](plugins/color_bulb/) | **L3** | Two simulated lights (`sim.kitchen_lamp`, `sim.living_lamp`). Devices-page widgets from the Host catalog. |
| [meter_plug](plugins/meter_plug/) | **L3** | One simulated metered switch (`sim.meter_plug`: power + V/A/W). |
| [zigbee_ota](plugins/zigbee_ota/) | **L3** | Claim Host Zigbee points (`props.owner`) + catalog OTA flag. One device stays `zigbee.zb_%04x`. ZCL 0x0019 air path is a Host stub. |
| [ble_mesh_ota](plugins/ble_mesh_ota/) | **L3** | Same claim/catalog pattern on Host BLE Mesh points. Mesh Firmware Update air path is a Host stub. |
| [matter_ota](plugins/matter_ota/) | **L3** | Same pattern on Host Matter endpoints (`light/matter_n*_ep*`). Hub runs CSA Provider + BDX when OTA is started. |

Radio OTA plugins **must not** `point_register` a second id. Enrich the Host
point with `merge_props` (`owner` / `ota`).

## Pack (development PC)

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack <id>    # or: pack all
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

On the Hub: Store `custom_url` → refresh → install. REST `kind` is `pack` for
L1, `module` for L2/L3. Then `POST /api/hub/reload`.

L3 also needs a matching `.so`. In-tree: export `MINIHUB_PLUGIN_DIRS` **before**
`./tools/build_firmware.py … build`. Without an IDF project (needs
`riscv32-esp-elf-gcc` and `cmake`; MHMD file is `plugins/<id>/build/<id>.so`):

```bash
python3 tools/build_plugin_standalone.py color_bulb
```

Then pack as above. L1/L2 zip only — no firmware compile.

Scaffold for a new module: `python3 tools/new_store_module.py --tier l1|l2|l3 --id …`
(default destination is `components/minihub_plugins/`; copy into `plugins/`
for a demo).

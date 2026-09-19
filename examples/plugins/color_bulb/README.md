# Example plugin: color_bulb (color bulbs, online sim)

[English](README.md) | [中文](README_CN.md)

Standalone L3 native demo under `examples/plugins/` (not in default
`application/minihub` or `components/minihub_plugins/`). Registers **two online
color bulbs** (`type=light`) and handles Web UI commands — **no hardware**.

Uses the Host `light` catalog (`minihub_device_widget/model` → runtime
`/data/config/devices.csv` + `/data/images`). Does **not** ship labels / widgets
/ CSS / icons in the plugin.

**One device = one point:** `sim.kitchen_lamp` and `sim.living_lamp`, with live
values in each point's `props.params`.

## Files

| File | Role |
|------|------|
| `module.json` | Manifest (`id=color_bulb`, runtime `native`) |
| `module_entry.c` | `minihub_module_entry` — adapter + Host API table |
| `color_bulb_adapter.c` | `start()` registers each `sim.<device>` + commands by `point_id` |
| `devices.json` | Array of instances: `device` / `type` / `params[]` (`name`, `init`, optional `access` / `visible` / bounds). Omitted bounds use catalog defaults. Packed into `.mhub` and factory-preinstall when present |
| `CMakeLists.txt` | IDF component via `MINIHUB_PLUGIN_DIRS`; packs to MHMD `.so` |
| `device_desc.gen.h` | Generated from `devices.json`; required before build; **do not edit** (embed fallback) |

## Build

Generate the header, then pull this tree into the Host configure:

```bash
python3 tools/generate_devices_property_from_json.py \
    examples/plugins/color_bulb/devices.json \
    examples/plugins/color_bulb/device_desc.gen.h

export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # or .../examples/plugins/color_bulb
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` is read at **configure** time
(`cmake/minihub_bundle/minihub_import.cmake`). It does not auto-preinstall.

Needs Dynamic Store Modules (`CONFIG_MINIHUB_DYNAMIC_MODULES`; default product
`dynamic_modules` feature).

### Install — factory preinstall (optional)

```yaml
# application/minihub/manifest.yml — temporary
store_preinstall: [color_bulb]
```

Rebuild/flash with `MINIHUB_PLUGIN_DIRS` set. Clear the list when done.

### Install — Store (no manifest change)

With `MINIHUB_PLUGIN_DIRS`, configure builds `libadapter_color_bulb.so` and can
stage `color_bulb.mhub`. Install from Store UI, or:

```bash
python3 tools/pack_store_module.py pack color_bulb
python3 tools/sync_store_catalog.py all
```

Store install / `module_attach` creates the `adapters.json` `color_bulb` slot
(`ensure_adapter_slot`). Without that slot, `load_all` skips the adapter.

After rebuilding the Host (control path + web UI), also reflash / restage the
web assets (`modstore-flash` or full `flash`) so Devices sends `point_id` for
each card.

## Change params

Edit `devices.json` → regenerate `device_desc.gen.h` → rebuild. Param names must
match the `light` catalog (`power`, `brightness`, `hue`, `saturation`, `cct`, …).

## Runtime

At `start()`, the adapter loads instance descriptors in this order:

1. Host `module_read_file(adapter, "devices.json")` — file next to the
   installed `module.json` / `.so` under the Store module dir (no hardcoded path)
2. Embedded `DEVICE_DESC_JSON` from `device_desc.gen.h` if the API is missing,
   the file is absent, or the adapter is host-linked without a Store install dir

Pack (`tools/pack_store_module.py`) and factory preinstall (modstore CMake)
include `devices.json` when the file exists in the module source tree.

- Overview: two online `light` cards (`sim.kitchen_lamp`, `sim.living_lamp`)
- Devices: kitchen has power + brightness / hue / saturation (`cct` hidden);
  living has power + brightness
- Control: `POST /api/devices/control` with `point_id` → adapter updates that
  instance's `props.params`

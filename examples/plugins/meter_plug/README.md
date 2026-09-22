# Example plugin: meter_plug (metered switch, online sim)

[English](README.md) | [中文](README_CN.md)

Standalone L3 native demo under `examples/plugins/` (not in default
`application/minihub`). Registers **one online
metered switch** (`type=switch`: power + voltage/current/power) — **no hardware**.

Uses the Host `switch` catalog. Does **not** ship labels / widgets / CSS / icons.

**One device = one point:** `sim.meter_plug` with values in `props.params`.

## Files

| File | Role |
|------|------|
| `module.json` | Manifest (`id=meter_plug`, runtime `native`) |
| `module_entry.c` | `minihub_module_entry` — adapter + Host API table |
| `meter_plug_adapter.c` | Registers `sim.meter_plug` + switch/meter simulation |
| `devices.json` | Instance: `type=switch`, params `power` / `voltage_v` / `current_a` / `power_w` |
| `device_desc.gen.h` | Generated from `devices.json`; **do not edit** |
| `CMakeLists.txt` | IDF component via `MINIHUB_PLUGIN_DIRS`; packs to MHMD `.so` |

## Behavior

- `point_id` = `sim.meter_plug`, `type` = `switch`, `adapter` = `meter_plug`
- `power` is RW; metering params use `access:"R"` (UI read-only)
- On: voltage ≈ 220 V, current ≈ 0.45 A, power ≈ V×I
- Off: current/power ≈ 0, line voltage still ≈ 220 V
- Commands: `turn_on` / `turn_off` / `toggle` / `set` (Host `point_set`)
- Needs `adapters.json` slot `meter_plug` (`ensure_adapter_slot` on Store install /
  `module_attach`)

## Build

### Standalone (no IDF)

```bash
python3 tools/build_plugin_standalone.py meter_plug
```

### In-tree with Host

```bash
python3 tools/generate_devices_property_from_json.py \
    examples/plugins/meter_plug/devices.json \
    examples/plugins/meter_plug/device_desc.gen.h

export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # or .../examples/plugins/meter_plug
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` is configure-time (`cmake/minihub_bundle/minihub_import.cmake`).

### Factory preinstall (optional)

```yaml
# application/minihub/manifest.yml — temporary
store_preinstall: [meter_plug]
```

Rebuild/flash with `MINIHUB_PLUGIN_DIRS`; clear the list afterward.

### Store install (no manifest change)

```bash
python3 tools/pack_store_module.py pack meter_plug
python3 tools/sync_store_catalog.py all
```

After flash + install, Overview / Devices show one switch card (`sim.meter_plug`).

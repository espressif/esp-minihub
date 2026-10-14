# Example plugin: color_bulb_js (L2 JS color bulbs)

[English](README.md) | [中文](README_CN.md)

Standalone **L2 QuickJS** demo under `examples/plugins/` (not in default
`application/minihub`). Registers **three
online simulated lights** (`type=light`) with Devices-page widgets and a JS
command handler — **no hardware**.

Companion to [`meter_plug_js`](../meter_plug_js/) (a switch-port of this JS
approach). This module replaces the old `color_bulb` L3 C demo (removed).

## When to use

| Need | Use |
|------|-----|
| Native `.so` + embedded `devices.json` | **L3** native plugin |
| Same UX from QuickJS (prototype / teaching) | **L2** this demo |
| Glue over existing Zigbee/L3 points | **L2** `auto_off` |

Needs Host firmware that includes the extended JS bindings in
`components/minihub_js/minihub_js.c` (`params`/`props` on register, optional
`props` on `point.set`, `host.command.on`). Older Hosts can still install the
module but control will 404 until those bindings are flashed.

## Runtime

On each `setup(host)` (boot + `POST /api/hub/reload`):

1. `host.command.on("color_bulb_js", handler)` — Core routes
   `POST /api/devices/control` / `POST /api/command` for points with
   `adapter=color_bulb_js` into JS.
2. Registers three points with `params` → Core `props.params`:

| point_id | Distinct init |
|----------|----------------|
| `sim.js_kitchen` | power off, brightness 80, hue 0 |
| `sim.js_desk` | power on, brightness 40, hue 120 |
| `sim.js_bedroom` | power on, brightness 60, hue 240 |

Param names match the Host `light` catalog (`power`, `brightness`, `hue`,
`saturation`, optional hidden `cct`). Labels/widgets come from the Host
device-widget catalog — the plugin does not ship CSS/icons.

Control path mirrors the L3 native approach: update `props.params.<name>.value`, then
`host.point.set(id, summary, { params })` so the UI refreshes.

## Files

| File | Role |
|------|------|
| `module.json` | Manifest (`id=color_bulb_js`, `runtime: js`) |
| `main.js` | `setup(host)` — register 3 lights + command handler |
| `README.md` / `README_CN.md` | This guide |

No `CMakeLists.txt` / `.so`. Instance list is embedded in `main.js` (L2 has no
`module_read_file`). Pack = zip of `module.json` + `main.js`.

## Pack & install (Store)

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack color_bulb_js
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

On the Hub: Store `custom_url` → refresh → install `color_bulb_js` →

```bash
curl -X POST http://$HUB/api/hub/reload
```

Host needs `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS`.
Public Host `store_preinstall` stays `[]`.

Store install creates an `adapters.color_bulb_js` slot when present; an empty
slot is enough (devices are registered from JS, not from `adapters.json`).

## Verify

* Serial: `JS module 'color_bulb_js' setup ok` and three `registered sim.js_*`
* `GET /api/points` — `sim.js_kitchen`, `sim.js_desk`, `sim.js_bedroom` with
  `type=light` and `props.params`
* Devices page: three light cards; toggles/sliders update values
* `POST /api/devices/control` with `point_id` + `parameter`/`value` succeeds
  (not 404)

## UI can / cannot

| | |
|--|--|
| **Can** | Display widgets; control power/brightness/hue/saturation; coexist with L2 sibling `meter_plug_js` |
| **Cannot** | Timers/events (`host.timer` / `point.updated` subscribe still missing); load external `devices.json` at runtime |

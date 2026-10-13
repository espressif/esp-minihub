# Example plugin: meter_plug_js (L2 JS smart plug)

[English](README.md) | [中文](README_CN.md)

Standalone **L2 QuickJS** demo under `examples/plugins/` (not in default
`application/minihub`). Registers **one online simulated smart plug**
(`type=switch`) with a writable `power` param and read-only `voltage` /
`current` / `watts` telemetry, plus a JS command handler — **no hardware**.

This is the JS (L2) replacement for the old `meter_plug` L3 C demo (removed from
the tree). Companion to [`color_bulb_js`](../color_bulb_js/) (the light-port of
the same approach).

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

1. `host.command.on("meter_plug_js", handler)` — Core routes
   `POST /api/devices/control` / `POST /api/command` for points with
   `adapter=meter_plug_js` into JS.
2. Registers one point with `params` → Core `props.params`:

| point_id | Params |
|----------|--------|
| `sim.js_plug` | `power` (RW), `voltage`/`current`/`watts` (RO) |

Param names match the Host `switch` catalog (`power`). `voltage` / `current` /
`watts` are declared `RO` and refuse client writes. Labels/widgets come from the
Host device-widget catalog — the plugin does not ship CSS/icons.

Control path mirrors the L3 native approach: update `props.params.<name>.value`,
then `host.point.set(id, summary, { params })` so the UI refreshes.

## Files

| File | Role |
|------|------|
| `module.json` | Manifest (`id=meter_plug_js`, `runtime: js`) |
| `main.js` | `setup(host)` — register plug + command handler |
| `README.md` / `README_CN.md` | This guide |

No `CMakeLists.txt` / `.so`. Instance list is embedded in `main.js` (L2 has no
`module_read_file`). Pack = zip of `module.json` + `main.js`.

## Pack & install (Store)

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack meter_plug_js
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

On the Hub: Store `custom_url` → refresh → install `meter_plug_js` →

```bash
curl -X POST http://$HUB/api/hub/reload
```

Host needs `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS`.
Public Host `store_preinstall` stays `[]`.

Store install creates an `adapters.meter_plug_js` slot when present; an empty
slot is enough (devices are registered from JS, not from `adapters.json`).

## Verify

* Serial: `JS module 'meter_plug_js' setup ok` and `registered sim.js_plug`
* `GET /api/points` — `sim.js_plug` with `type=switch` and `props.params`
* Devices page: one switch card; power toggles update the value
* `POST /api/devices/control` with `point_id` + `parameter`/`value` succeeds
  (not 404); writing `voltage`/`current`/`watts` returns an error

## UI can / cannot

| | |
|--|--|
| **Can** | Display widgets; toggle power; coexist with L2 sibling `color_bulb_js` |
| **Cannot** | Timers/events subscribe; load external `devices.json` at runtime; write read-only telemetry |

# Example plugin: ble_mesh_pack (L1 config pack)

[English](README.md) | [中文](README_CN.md)

Standalone **L1 pack** under `examples/plugins/` (not in default
`application/minihub`). It ships **only**
`module.json`: no `main.js`, no `.so`, no radio `devices[]`.

Host `minihub_pack` **merges** `inject` into the existing `adapters.ble_mesh`
block on load / `POST /api/hub/reload`. Public Host `store_preinstall` stays
`[]`.

## When to use

| Need | Use |
|------|-----|
| Defaults for a **Host** adapter (bind list, UUID filters, Modbus maps) | **L1** this demo |
| Logic, HTTP, or a new adapter | **L2** / **L3** |
| Vendor claim / OTA on a live radio point | **L3** native module (same `point_id`) |

Do **not** inject `devices[]` on `zigbee` / `matter` / `ble_mesh`: those points
are registered when a node joins. Whatever the target adapter, Host skips a pack
whose block is already present with `enabled: false`.

## What it merges

Appends one SIG bind entry that factory `adapters.json` does **not** list:

| Field | Value |
|-------|--------|
| `model_id` | `0x1307` (Light HSL Server) |
| `name` | `l1_demo_light_hsl_srv` (marker so you can spot the merge) |

Host BLE Mesh **clients today** bind OnOff / Level / Lightness / CTL. This extra
row is for teaching inject merge, not a new lighting pipeline. It does **not**
restrict UUID matching (`uuid_filters` stay empty → still accept any
unprovisioned node).

If `adapters.ble_mesh.enabled` is **false**, Host **skips** the pack (serial:
`skip pack 'ble_mesh_pack'`).

## Files

| File | Role |
|------|------|
| `module.json` | Manifest (`type`/`runtime`: `pack`) + `inject` |
| `README.md` / `README_CN.md` | This guide |

Pack = zip of `module.json` only. After Store install, Host copies it to
`/data/config/packs/ble_mesh_pack.json`.

## Pack & install (Store)

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack ble_mesh_pack
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

On the Hub: Store `custom_url` → refresh → install. REST:

```bash
curl -X POST http://$HUB/api/store/install \
  -H 'Content-Type: application/json' \
  -d '{"type":"pack","id":"ble_mesh_pack"}'
curl -X POST http://$HUB/api/hub/reload
```

## Verify

* Serial: `loaded pack 'BLE Mesh L1 defaults (demo)' (ble_mesh_pack)` then
  `merged pack 'ble_mesh_pack' into adapter 'ble_mesh'`
* `GET /api/packs` — `id=ble_mesh_pack`, `inject_adapter=ble_mesh`
* `GET /api/config/adapters` — `adapters.ble_mesh.provision.bind_models`
  includes `{ "model_id": "0x1307", "name": "l1_demo_light_hsl_srv" }`
* Devices page is **unchanged** until a real Mesh node joins (L1 does not
  register points)

## UI can / cannot

| | |
|--|--|
| **Can** | Patch Host BLE Mesh provision defaults without rebuilding firmware |
| **Cannot** | Register a Mesh device, run JS, `dlopen`, claim `owner` / OTA |

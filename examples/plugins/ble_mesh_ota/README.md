# Example plugin: ble_mesh_ota (cloud catalog + firmware cache)

[English](README.md) | [中文](README_CN.md)

Standalone **L3 native** demo under `examples/plugins/` (not in default Host
`store_preinstall`). It does **not** register devices. Host BLE Mesh already
owns one point per node (`light/bm_%04x`). This plugin:

1. Lists Host points with `adapter=ble_mesh` (`minihub_point_list_by_adapter`)
2. Keeps Espressif-only filter + `catalog_url` in packed `catalog.json`
3. On a manufacturer match, writes `props.owner=ble_mesh_ota` on that same point. Overview moves the card under the vendor section; `point.adapter` stays `ble_mesh` so radio commands still hit the Host
4. **GET `catalog_url`** for offered `version` + `firmware_url` (not baked into the plugin)
5. Optionally streams firmware to `/data/ble_mesh_ota/<uuid>.bin`
6. Merges `props.ota` on the **same** `point_id`

The Devices UI shows an OTA button when `props.ota.available` is true.
`POST /api/devices/ota` still hits the BLE Mesh adapter; **Mesh Firmware
Update / BLOB air transport is a Host stub** (`ESP_ERR_NOT_SUPPORTED`). This
demo stops at the file on LittleFS plus the UI flag.

L2 JS cannot do this: `host.http.request` caps the body at 16 KiB and has no
file API.

## Files

| File | Role |
|------|------|
| `module.json` | Manifest (`id=ble_mesh_ota`, runtime `native`) |
| `module_entry.c` | `minihub_module_entry` — adapter + Host vtable |
| `ble_mesh_ota_adapter.c` | Scan / download / `point_merge_props` |
| `catalog.json` | Local: `catalog_url` + `manufacturer` (no image version) |
| `ui.json` | Overview section label (`filter_label`: Espressif) |
| `CMakeLists.txt` | IDF component via `MINIHUB_PLUGIN_DIRS` |

## Build

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # or .../examples/plugins/ble_mesh_ota
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` is read at **configure** time. It does not auto-preinstall.
Needs `CONFIG_MINIHUB_DYNAMIC_MODULES`.

Do **not** add `ble_mesh_ota` to public Host `store_preinstall`.

### Install — Store

```bash
python3 tools/pack_store_module.py pack ble_mesh_ota
python3 tools/sync_store_catalog.py all
```

Store install creates the `adapters.json` `ble_mesh_ota` slot. `catalog_url` in
packed `catalog.json` is used unless `adapters.json` overrides it.

Cloud `ota-catalog.json` (what the plugin GETs):

```json
{
  "devices": [
    {
      "manufacturer": "Espressif",
      "version": "1.0.0",
      "firmware_url": "http://192.168.1.100:8080/1.0.0.bin"
    }
  ]
}
```

`version` / `firmware_url` come **only** from that response (`props.ota.version`
is the offered image, not `props.sw_version`). No cloud catalog → no OTA flag.
Matching is case-insensitive on Composition CID manufacturer (`0x02E5` →
`Espressif`). Devices without `props.manufacturer` yet are skipped.

RPC `POST /api/modules/ble_mesh_ota/rpc` methods: `refresh`, `download`.

## Host exports used

`mkdir` / `stat` / `unlink` / `rmdir`, streaming `esp_http_client_open` +
`read`, `minihub_point_list_by_adapter`, `minihub_point_merge_props`. BLE Mesh
radio APIs are **not** exported to plugins.

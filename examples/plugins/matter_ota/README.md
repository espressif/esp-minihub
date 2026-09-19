# Example plugin: matter_ota (cloud catalog + firmware cache)

[English](README.md) | [中文](README_CN.md)

Standalone **L3 native** demo under `examples/plugins/` (not in default Host
`store_preinstall`). It does **not** register devices. Host Matter already
owns one point per commissioned endpoint (`light/matter_n*_ep*`). This plugin:

1. Lists Host points with `adapter=matter` (`minihub_point_list_by_adapter`)
2. Keeps Espressif-only filter + `catalog_url` in packed `catalog.json`
3. On a manufacturer match, writes `props.owner=matter_ota` on that same point. Overview moves the card under the vendor section; `point.adapter` stays `matter` so radio commands still hit the Host
4. **GET `catalog_url`** for offered `version` + `firmware_url` (not baked into the plugin)
5. Optionally streams firmware to `/data/matter_ota/<node_id>.bin`
6. Merges `props.ota` on the **same** `point_id`

The Devices UI shows an OTA button when `props.ota.available` is true.
`POST /api/devices/ota` hits the Matter adapter; Host runs the CSA path
(Provider `0x0029`, `AnnounceOTAProvider`, BDX). The cached file must be a
Matter OTA image (not a raw ESP firmware `.bin`).

Thread end-devices are Matter points (not `adapter=thread`).

## Build

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # or .../examples/plugins/matter_ota
export ESP_MATTER_PATH=/path/to/esp-matter
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` is read at **configure** time. It does not auto-preinstall.
Needs `CONFIG_MINIHUB_DYNAMIC_MODULES`.

Do **not** add `matter_ota` to public Host `store_preinstall`.

```bash
python3 tools/pack_store_module.py pack matter_ota
```

Matching is case-insensitive on Basic Information VendorName. Devices without
`props.manufacturer` yet are skipped.

RPC `POST /api/modules/matter_ota/rpc` methods: `refresh`, `download`.

# Build

## Product = application folder

| Concept | Meaning |
|---------|---------|
| **Product** | `application/<name>/`, selected by `-app` / `MINIHUB_APP` (`minihub`, `claw_minihub`, …) |
| **Product manifest** | `application/<app>/manifest.yml` (board / features / adapters / `store_preinstall`; may `extends: _host_base`) |
| **Shared Host base** | `config/minihub_manifest.yml` (`profiles._host_base` only) |
| **Runtime config seed** | `application/<app>/config/{adapters,rules,scenes}.json` → else repo `config/` |
| **Sdkconfig recipe** | `application/<app>/config/sdkconfig/app.yml` (one per product) |

Sync: `MINIHUB_APP=<app> python tools/sync_host_manifest.py`. New Host-style products add an app folder + `manifest.yml`; do not edit the shared base unless changing Core requires. Claw-style apps set `hand_maintained: true` to skip generated `idf_component.yml` / `adapters_setup`. Factory Store modules: `store_preinstall: [<id>, …]`.

There is no separate `MINIHUB_PROFILE`. Optional HW recipes live under `variants/` (sdkconfig) or `manifest.yml` → `variants.lcd`; apply sdkconfig variants with `tools/apply_sdkconfig_variant.sh`.

```bash
./tools/build_firmware.py -app minihub zigbee build
MINIHUB_APP=minihub ./tools/build_firmware.py build
```

Compose: `tools/compose_sdkconfig.py`. Stacks file: `cmake/generated/sdkconfig_stacks.gen` (gitignored).

## Stacks

`zigbee` · `ble_mesh` · `matter` (args to `build_firmware.py`). Default: `zigbee` + `ble_mesh`.

Matter is **not** in the default stack (needs `ESP_MATTER_PATH` and extra flash/RAM). Enable Wi-Fi Matter devices:

```bash
export ESP_MATTER_PATH=/path/to/esp-matter   # or use build_firmware default bootstrap
./tools/build_firmware.py -app minihub matter build
./tools/build_firmware.py -app claw_minihub matter build
```

**Partition note:** default OTA app slots (e.g. claw `partitions.csv` 5 MiB) are often too small for a Matter commissioner image (~5.3 MiB+). Do **not** treat shrinking FAT/modstore in-tree as the product fix — enlarge `ota_0`/`ota_1` (or drop other stacks) in a local/custom partition table if the link step reports the binary does not fit, then flash that table once.

Runtime: set `adapters.matter.enabled: true` in `adapters.json` (product seeds keep Matter / BLE Mesh **off** by default). Commission via Web UI or MCP/API:

| Path | Purpose |
|------|---------|
| `GET /api/matter/status` | Commissioner ready / pairing state |
| `POST /api/matter/commission` | Pair with `pairing_code` or `setup_pin` |
| `POST /api/matter/devices` | Commission + upsert endpoints into `adapters.json` |

Discovery modes (body field `discovery`):

- `network` (default) — multi-admin / device already on LAN (e.g. Google Home “Add to other services”)
- `ble_wifi` / `first_time` — Hub BLE + inject **device** home Wi-Fi (`wifi_ssid`, `wifi_password`)

Hub uplink can be Ethernet while provisioning a Wi-Fi Matter device (`matter_over_eth` fragment when `eth` feature is on). Control today: **OnOff lights** only (`turn_on` / `turn_off` / `toggle`).

## CI

`tools/ci/` + `.gitlab-ci.yml` (`MINIHUB_APP`).

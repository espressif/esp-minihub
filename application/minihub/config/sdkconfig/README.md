# minihub product sdkconfig

**Product** = this folder (`minihub`). Selected via `-app minihub` / `MINIHUB_APP`.

| Path | Role |
|------|------|
| `app.yml` | Board + feature recipe for this product |
| `feature/*.defaults` | Capability fragments |
| `variants/*.yml` | Optional HW recipes (e.g. LCD); `./tools/apply_sdkconfig_variant.sh lcd` |
| `local.overrides` | Lab SSID (gitignored) |

Shared fragments: repo-root `config/sdkconfig/`.

```bash
./tools/build_firmware.py -app minihub build              # reconcile stacks with sdkconfig
./tools/build_firmware.py -app minihub zigbee build       # force-enable Zigbee if not disabled
./tools/build_firmware.py -app minihub ble_mesh build     # force-enable BLE Mesh if not disabled
# Optional LCD HW recipe:
# ./tools/apply_sdkconfig_variant.sh lcd
```

Boot order: BLE Mesh / Zigbee adapters start before `esp_wifi_init()` (see `minihub_net_start_wifi`). Radio memory trim: `config/sdkconfig/feature/radio_coexist_memory.defaults`.

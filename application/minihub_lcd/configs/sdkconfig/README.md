# minihub_lcd product sdkconfig

**Product** = `minihub_lcd`. Selected via `-app minihub_lcd` / `MINIHUB_APP`.

| Path | Role |
|------|------|
| `app.yml` | Fixed board + feature recipe for this product (LCD only, no `eth`) |
| `feature/*.defaults` | Capability fragments; `lcd.defaults` selects `partitions.csv` |
| `local.overrides` | Lab SSID (gitignored) |

There is deliberately **no** `variants/` directory here: this product is the LCD
recipe itself. Switching hardware recipes belongs to `application/minihub`.

Shared fragments: repo-root `configs/sdkconfig/`.

```bash
./tools/build_firmware.py -app minihub_lcd build                # reconcile stacks with sdkconfig
./tools/build_firmware.py -app minihub_lcd zigbee build         # force-enable Zigbee if not disabled
./tools/build_firmware.py -app minihub_lcd ble_mesh build       # force-enable BLE Mesh if not disabled
```

`sdkconfig` under `application/minihub_lcd/` is generated from this recipe and is
not shared with other products. Boot order: BLE Mesh / Zigbee adapters start before
`esp_wifi_init()` (see `minihub_net_start_wifi`). Radio memory trim:
`configs/sdkconfig/feature/radio_coexist_memory.defaults`.

Thread is forbidden here (`feature/lcd.defaults`): no `MINIHUB_THREAD_ADAPTER`,
no `OPENTHREAD_ENABLED`. Use the ETH product when you need OpenThread BR.

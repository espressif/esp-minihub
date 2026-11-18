# minihub_lcd (LCD-only Host)

**Product:** `minihub_lcd`. Select with `-app minihub_lcd` / `MINIHUB_APP=minihub_lcd`.

This product exists because the LCD UI and Ethernet are **mutually exclusive**.
`CONFIG_MINIHUB_LCD_UI` depends on `!CONFIG_MINIHUB_NET_ETH`: EMAC and the RGB LCD
share pin groups on `esp32_s31_korvo1`, and the Function CoreBoard has RJ45 and no
LCD. `application/minihub` keeps the Ethernet-oriented recipe. This product is
fixed to LCD and never needs `./tools/apply_sdkconfig_variant.sh`.

Reference board: `esp32_s31_korvo1` (800x480 RGB + GT1151 touch).
Partition table: `partitions.csv` — the same 16 MB layout as
`application/minihub/partitions.csv` (single `ota_0` plus a `vibe_mode`
factory image for external-storage OTA).

## Build

```bash
./tools/build_firmware.py -app minihub_lcd build flash monitor

cp application/minihub_lcd/configs/sdkconfig/local.overrides.example \
   application/minihub_lcd/configs/sdkconfig/local.overrides   # optional lab Wi-Fi
```

`sdkconfig` is per-product (`application/minihub_lcd/sdkconfig`) and is generated
from `configs/sdkconfig/app.yml`. Do not copy an `sdkconfig` from another product.

## Notes

- `tools/sync_host_manifest.py` writes generated files into shared
  `cmake/generated/` and `application/<app>/main/{idf_component.yml,adapters_setup.generated.c}`.
  Build products serially; each build re-runs the sync.
- Radio stacks: `zigbee` · `ble_mesh` · `matter`. Thread is **off** for this
  product (`lcd.defaults` clears `MINIHUB_THREAD_ADAPTER` / `OPENTHREAD_ENABLED`)
  — Wi-Fi STA uplink cannot link OT BR beside Matter CHIP route hooks.
- Store / plugins: see [docs/en/plugins/index.rst](../../docs/en/plugins/index.rst).

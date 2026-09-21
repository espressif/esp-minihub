# minihub (generic Host)

**Product:** `minihub` (this directory). Select with `-app minihub` / `MINIHUB_APP=minihub`.

Default recipe `config/sdkconfig/app.yml`: Ethernet-oriented (Wi-Fi STA fallback, USB MSC, RTSP, Modbus, …). Reference board `esp32_s31_function_coreboard_1`.

Optional LCD recipe `config/sdkconfig/variants/lcd.yml`:

```bash
./tools/apply_sdkconfig_variant.sh lcd
./tools/build_firmware.py -app minihub build
# Restore default:
./tools/apply_sdkconfig_variant.sh --restore
```

## Build

```bash
./tools/build_firmware.py -app minihub build flash monitor

cp application/minihub/config/sdkconfig/local.overrides.example \
   application/minihub/config/sdkconfig/local.overrides   # optional lab Wi-Fi
```

Radio stacks: `zigbee` · `ble_mesh` · `matter`. See [docs/en/getting_started.rst](../../docs/en/getting_started.rst).

## Store / plugins

See [docs/en/plugins/index.rst](../../docs/en/plugins/index.rst).

```bash
./tools/build_firmware.py -app minihub modules
./tools/build_firmware.py -app minihub pack
python3 tools/pack_store_module.py pack all
```

# Build

## Product = application folder

| Concept | Meaning |
|---------|---------|
| **Product** | `application/<name>/`, selected by `-app` / `MINIHUB_APP` (`minihub`, `claw_minihub`, …) |
| **Sdkconfig recipe** | `application/<app>/config/sdkconfig/app.yml` (one per product) |

There is no separate `MINIHUB_PROFILE`. Optional HW recipes live under `variants/`; apply with `tools/apply_sdkconfig_variant.sh`.

```bash
./tools/build_firmware.py -app minihub zigbee build
MINIHUB_APP=minihub ./tools/build_firmware.py build
```

Compose: `tools/compose_sdkconfig.py`. Stacks file: `cmake/generated/sdkconfig_stacks.gen` (gitignored).

## Stacks

`zigbee` · `ble_mesh` · `matter` (args to `build_firmware.py`). Default: `zigbee` + `ble_mesh`.

## CI

`tools/ci/` + `.gitlab-ci.yml` (`MINIHUB_APP`).

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

## CI

`tools/ci/` + `.gitlab-ci.yml` (`MINIHUB_APP`).

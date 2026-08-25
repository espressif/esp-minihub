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
./tools/build_firmware.py -app minihub zigbee build
# Optional LCD HW recipe:
# ./tools/apply_sdkconfig_variant.sh lcd
```

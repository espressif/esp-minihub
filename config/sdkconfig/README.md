# Shared MiniHub sdkconfig (all apps)

| Path | Role |
|------|------|
| `common.defaults` | Host must-haves (HTTPD, Web UI, partition table flag, …) |
| `memory.defaults` | ESP32-S31 dual-radio / PSRAM memory policy |
| `feature/` | Shared radio stacks (`zigbee`, `ble_mesh`, `matter*`) |

Build-time stack disable list is written to `cmake/generated/sdkconfig_stacks.gen` (gitignored), not under this tree.

App-specific recipe: `application/<app>/config/sdkconfig/app.yml`.

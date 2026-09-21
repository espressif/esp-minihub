# Shared MiniHub sdkconfig (all apps)

| Path | Role |
|------|------|
| `common.defaults` | Host must-haves (HTTPD, Web UI, partition table flag, …) |
| `memory.defaults` | ESP32-S31 dual-radio / PSRAM memory policy |
| `feature/` | Shared capability + radio fragments (`eth`, `wifi_sta`, `dynamic_modules`, `zigbee`, `ble_mesh`, `radio_coexist_memory`, `matter*`, …) |

`radio_coexist_memory` — optional NimBLE/BT-controller trim + higher `SPIRAM_MALLOC_RESERVE_INTERNAL`; auto-applied when building with `zigbee` and/or `ble_mesh` stacks. Override in `application/<app>/config/sdkconfig/local.overrides`.

S31 XIP: keep `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` unset in `memory.defaults` (same as a working Host `sdkconfig`). Enabling it shares `drom` between flash rodata and `.ext_ram.dummy` and can fail link with `cannot move location counter backwards`.

Build-time stack disable list is written to `cmake/generated/sdkconfig_stacks.gen` (gitignored), not under this tree.

App-specific recipe: `application/<app>/config/sdkconfig/app.yml`.

# Shared MiniHub sdkconfig (all apps)

| Path | Role |
|------|------|
| `common.defaults` | Host must-haves (HTTPD, Web UI, partition table flag, …) |
| `memory.defaults` | ESP32-S31 dual-radio / PSRAM memory policy |
| `feature/` | Shared capability + radio fragments (`eth`, `wifi_sta`, `dynamic_modules`, `zigbee`, `ble_mesh`, `radio_coexist_memory`, `matter*`, …) |

`radio_coexist_memory` — optional NimBLE/BT-controller trim + higher `SPIRAM_MALLOC_RESERVE_INTERNAL`; auto-applied when building with `zigbee` and/or `ble_mesh` stacks. Override in `application/<app>/configs/sdkconfig/local.overrides`.

S31 XIP: `memory.defaults` sets `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`. `flash_rodata_seg` and `ext_ram_seg` share `drom`; if `.ext_ram.dummy` rewinds past `.flash.rodata_noload`, the link fails with `cannot move location counter backwards`.

L3 Store modules: `memory.defaults` sets `CONFIG_SPIRAM_XIP_FROM_PSRAM` (selects fetch/rodata/flash-load); `feature/dynamic_modules.defaults` sets `CONFIG_ELF_LOADER_LOAD_PSRAM`. Do not rely on a hand-edited `application/<app>/sdkconfig` alone for these.

Build-time stack disable list is written to `cmake/generated/sdkconfig_stacks.gen` (gitignored), not under this tree.

App-specific recipe: `application/<app>/configs/sdkconfig/app.yml`.

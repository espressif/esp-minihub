# Thread TBR Implementation Plan

> **For agentic workers:** Implement task-by-task. Checkboxes track progress.

**Goal:** OpenThread Border Router stack + management (Web/MCP/LCD) + Matter transport / BLE_THREAD commission hooks.

**Architecture:** `minihub_thread` owns OT/TBR; Matter remains commissioner above RF; `adapters.thread.enabled` enables full BR; compile-time mutex with zigbee.

**Tech Stack:** ESP-IDF OpenThread BR, existing MiniHub web/MCP/LCD/matter patterns.

## Global Constraints

- Branch naming: underscores only
- No Cursor git attribution
- Host stays ecosystem-agnostic (no vendor tokens)
- Config key `adapters.thread` for RF; Matter devices only under `adapters.matter`

---

## File map

| Path | Responsibility |
|------|----------------|
| `components/minihub_thread/` | OT/TBR lifecycle, dataset, status JSON |
| `config/sdkconfig/feature/thread.defaults` | OT+BR+native radio+coex |
| `tools/build_firmware.py` | stack `thread`, zigbee mutex, stacks.gen |
| `application/*/Kconfig*` | THREAD without forcing Matter |
| `minihub_config.c` / `matter_sync.c` | legacy devices migrate; keep RF thread |
| `minihub_web.c` | `/api/thread/*` |
| `hub_api_gateway.c` | MCP allowlist |
| `minihub_lcd_app.cpp` | Matter filter=`matter`; Thread status; transport label |
| `minihub_matter` | `BLE_THREAD`, `transport` on upsert |
| manifests / `adapters.json` | require component + seed `thread` |

---

## Tasks

### Task 1: Build stack + Kconfig + defaults
- [ ] Add `thread` to `STACKS`; reject with `zigbee`
- [ ] `write_stacks_gen` toggles `CONFIG_MINIHUB_THREAD_ADAPTER`
- [ ] `thread.defaults`; wire in `sdkconfig_defaults` (+ coexist memory)
- [ ] Kconfig: THREAD is BR/radio; **do not** `select MINIHUB_MATTER_ADAPTER`
- [ ] Manifest require `minihub_thread`; adapters entry with kconfig

### Task 2: `minihub_thread` component
- [ ] Header API: init/start/stop/ready/status_json/dataset get/set/active_tlvs
- [ ] Real impl when `CONFIG_MINIHUB_THREAD_ADAPTER`; stub otherwise
- [ ] Boot: after net IP; form or join from config

### Task 3: Config migration + seed JSON
- [ ] Migrate only legacy `devices`; keep `thread` RF object
- [ ] Seed `thread` in repo + app configs

### Task 4: Web + MCP + main bringup
- [ ] HTTP handlers; MCP rules; call init from minihub/claw bringup

### Task 5: LCD + Matter transport / BLE_THREAD
- [ ] Overview filter `matter`; optional transport badge
- [ ] Thread status chip/toggle (thin)
- [ ] Discovery `ble_thread` + upsert `transport`

### Task 6: Docs
- [ ] Update `docs/build.md` stacks line

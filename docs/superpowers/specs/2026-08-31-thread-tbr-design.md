# Thread Border Router + Matter transport design

## Goal

Add OpenThread **Border Router** as a first-class RF stack on MiniHub (`minihub` + `claw_minihub`), then let Matter commission/control Thread devices while still presenting them as **Matter** points with an optional **transport** label (`wifi` / `ethernet` / `thread`).

## Decisions

| Topic | Choice |
|-------|--------|
| Phase order | **1a** OT/TBR + management surfaces; **1b** Matter `pairing_code_thread` + `transport` |
| RF vs Matter | Thread / Wi-Fi / Zigbee / BLE Mesh are RF; **Matter sits above RF** |
| Compile mutex (phase 1) | `thread` stack **mutex with `zigbee`** (shared 802.15.4) |
| Future RF policy | Same binary may compile multiple RFs; runtime enable one — `minihub_thread` stays start/stop/status only |
| Config key | `adapters.thread` — **default role = BR**; `enabled=true` turns on full OT+TBR related options |
| Devices | Never store Matter devices under `thread`; legacy `thread.devices` migrates to `matter` and is stripped |
| Display | Adapter name always `matter`; UI may show transport badge |

## Config schema

```json
"thread": {
  "enabled": false,
  "dataset": null,
  "channel": null
},
"matter": {
  "enabled": true,
  "devices": [
    {
      "point_id": "light/mt_…",
      "node_id": "…",
      "endpoint_id": 1,
      "transport": "thread"
    }
  ]
}
```

- No dataset → form network and persist dataset TLVs (hex/base64 in JSON).
- Dataset present → apply / join.
- `transport`: `wifi` | `ethernet` | `thread` | omit/`unknown`.

## Components

- **`minihub_thread`**: OpenThread + BR; dataset get/set; status; start/stop.
- **Stack `thread`**: `CONFIG_MINIHUB_THREAD_ADAPTER` + OT/TBR sdkconfig fragment.
- **No Hub points adapter** for Thread RF in phase 1.
- **Matter**: discovery `BLE_THREAD`; persist `transport` on upsert.

## APIs

| Method | Path | Role |
|--------|------|------|
| GET | `/api/thread/status` | role, attached, channel, dataset_set, last_error |
| POST | `/api/thread/start` / `stop` | enable/disable (persist `enabled`) |
| GET/POST | `/api/thread/dataset` | export / import (or generate if empty POST) |
| Matter | existing + `discovery=ble_thread`, devices include `transport` |

MCP allowlist mirrors these. LCD: Thread net status/toggle; Matter filter uses adapter `matter` + transport label.

## Non-goals (phase 1)

- Runtime Zigbee↔Thread switching in one firmware
- Thread as a separate Hub points adapter name
- Full Thread network UI (topology map, etc.)

## Acceptance

1. `./tools/build_firmware.py -app minihub thread matter build` succeeds (no `zigbee` together).
2. `zigbee` + `thread` rejected by `build_firmware.py`.
3. `adapters.thread.enabled=true` starts OT/TBR after net has IP.
4. Legacy `thread.devices` migrates to `matter` without deleting RF `thread` object.
5. Matter devices list/filter as `matter`; optional transport shown.
6. MCP/Web expose thread status and dataset start/stop.

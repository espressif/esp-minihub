# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Plugin-author knowledge served by the IDE MCP (keep aligned with AGENTS.md skills)."""

from __future__ import annotations

TOPICS = (
    "overview",
    "l1",
    "l2",
    "l3",
    "js_api",
    "pack",
    "constraints",
)

GUIDES: dict[str, str] = {
    "overview": """# MiniHub Store plugins (IDE MCP)

This MCP is for **plugin authors**. It is not the Hub on-device MCP (`CONFIG_MINIHUB_MCP_SERVER`).

| Tier | Files | Artifact |
|------|--------|----------|
| L1 pack | `module.json` (+ `inject`) | `<id>.mhub` zip |
| L2 JS | + `main.js` (+ optional `ui.json` / `web/`) | `<id>.mhub` |
| L3 native | + MHMD `.so` | `<id>.mhub` (needs Host build) |

`id` = directory name: `^[a-z][a-z0-9_]{0,30}$` (underscores, **no hyphens**).
`kind` is only `"module"` or `"pack"`.

Use tools: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin` (L1/L2).
Start from `docs/module_sdk/templates/`, not an existing vendor plugin.
Vendor cloud tokens stay in the plugin, never in Host Core.
""",
    "l1": """# L1 pack

Config only. No `main.js`, no `CMakeLists.txt`, no `.so`.

- `kind`: `"pack"`, `runtime`: `"pack"` (or `config_only: true`)
- `inject.adapter` must be an **existing Host** adapter: mqtt, zigbee, ble_mesh, modbus, matter, thread
- L1 does **not** add a new radio stack (that is Host or L3)

Install: pack `.mhub` → catalog URL on the Hub → refresh + install.
Store config field is `custom_url`, not `url`.
""",
    "l2": """# L2 JavaScript

Needs Host `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS`.

- `kind`: `"module"`, `runtime`: `"js"`
- Entry: `main.js` (QuickJS). Optional `ui.json` / `web/`
- Only call documented `host.*` APIs (see topic `js_api`)
- Do **not** call native Wi-Fi / Zigbee / BLE Mesh / OpenThread APIs from JS
- `host.command.invoke` is `{ point_id, cmd }` only — **no `params` today**
""",
    "l3": """# L3 native

Requires a MiniHub **Host** tree + IDF ~6.2 / `esp32s31`. This MCP cannot compile `.so`.

- Export `minihub_module_entry(host, config, out_adapter)`
- Optional `minihub_module_rpc`
- Call Core only via `minihub_module_host_api_t` (honor `struct_size`)
- `REGISTER_ADAPTER` only inside `#if !CONFIG_MINIHUB_DYNAMIC_MODULES`
- `abi_version` / `core_api_min` / `core_api_max` must match Host
- Undefined ELF symbols ⊆ `docs/module_sdk/host_export_allowlist.json`
- Link `-Wl,-z,max-page-size=4`
- No `#if CONFIG_*` for Host feature flags inside the plugin
- Start from the L3 template, not an existing vendor adapter

Load guard: `/store/modules/<id>/.load_guard`. Clear with
`POST /api/modules` `{"id":"...","clear_load_guard":true,"reload":true}`.
""",
    "js_api": """# L2 host.* API (from minihub_js.c — do not invent)

```
host.log.info(msg)
host.log.warn(msg)
host.log.error(msg)

host.point.register({ point_id, type?, adapter?, value?, online? })
  // type default "sensor"; adapter default "js"
host.point.get(point_id) → { point_id, type, value, adapter, online } | null
host.point.set(point_id, value)

host.command.invoke({ point_id, cmd })   // no params field

host.http.request({ url, method?, body?, timeout_ms? })
  → { status, body }
  // method: GET (default), POST, PUT

host.config.get() → adapters JSON (or null)
```

Do not add `host.wifi`, `esp_wifi_*`, or extra command fields.
""",
    "pack": """# Pack and install

Outer package is zip `.mhub` (Host `hub_zip.c`). MHMD is the **inner** native image, not the zip format.

L1/L2 from this MCP: tool `pack_plugin` on a module directory.

In the firmware repo:

```
python3 tools/pack_store_module.py pack <id>
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py
```

Point Hub Store at the catalog (`custom_url`), refresh, install.
There is no documented multipart `file=@*.mhub` install unless the running firmware says so.

L3: build Host first so `application/minihub/build/modules/libadapter_<id>.so` / MHMD `.so` exists, then pack.
""",
    "constraints": """# Constraints

- Plugin ids and git branches: underscores, not hyphens
- Host product = `application/<name>/`; Core = `components/` (no vendor rules in Core)
- Public Host must not bake vendor cloud tokens or default vendor devices into `config/` seeds
- IDF ~6.2 + preview target esp32s31; IDF 5.x has no S31 toolchain
- On-device MCP (`CONFIG_MINIHUB_MCP_SERVER` + esp-claw) is optional and unrelated to this IDE MCP
- REST is enough for a PC agent to talk to a running Hub
""",
}


def guide(topic: str) -> str:
    key = (topic or "overview").strip().lower()
    if key not in GUIDES:
        known = ", ".join(TOPICS)
        return f"unknown topic {topic!r}. Use one of: {known}"
    return GUIDES[key]

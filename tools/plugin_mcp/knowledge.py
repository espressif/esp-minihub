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
`type` is only `"module"` or `"pack"`.

Use tools: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin` (L1/L2).
Start from `docs/module_sdk/templates/`, not an existing vendor plugin.
Vendor cloud tokens stay in the plugin, never in Host Core.
""",
    "l1": """# L1 pack

Config only. No `main.js`, no `CMakeLists.txt`, no `.so`.

- `type`: `"pack"`, `runtime`: `"pack"` (or `config_only: true`)
- `inject.adapter` names the `adapters.json` block to merge into. The bundled
  adapters are mqtt, zigbee, ble_mesh, modbus, matter, thread, wifi; the merge
  creates an unknown key and enables it, but nothing talks to it unless an
  L2/L3 module registers that adapter name.
- L1 does **not** add a new radio stack (that is Host or L3)
- Merge rules: `devices[]` is appended; `provision.uuid_filters` /
  `provision.bind_models` are appended; every other key is replaced whole
- A pack is skipped when the target adapter block exists with `enabled: false`

Install: pack `.mhub` → catalog URL on the Hub → refresh + install.
Store config field is `custom_url`, not `url`.
""",
    "l2": """# L2 JavaScript

Needs Host `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS`.

- `type`: `"module"`, `runtime`: `"js"` (both required — the loader does not
  infer JS from `main.js`, and it rejects a manifest with no `adapter`)
- Entry: `main.js` must define a global `setup(host)`; `host` is not a global
- Optional `meta.json` for `ui` / `capabilities` (falls back to module.json)
- Only call documented `host.*` APIs (see topic `js_api`)
- Do **not** call native Wi-Fi / Zigbee / BLE Mesh / OpenThread APIs from JS
  (unreachable anyway: JS can only reach the registered `host.*` bindings)
- `host.command.invoke({ point_id, cmd, params? })` — `params` is passed to the
  adapter; blocking `host.http.request` occupies the single JS worker
""",
    "l3": """# L3 native

Requires a MiniHub **Host** tree + IDF ~6.2 / `esp32s31`. This MCP cannot compile `.so`.

- Export `minihub_module_entry(host, config, out_adapter)`
- Optional `minihub_module_rpc`
- Call Core only via `minihub_module_host_api_t` (honor `struct_size`)
- `REGISTER_ADAPTER` only inside `#if !CONFIG_MINIHUB_DYNAMIC_MODULES`
- `abi_version` must match Host; `core_api_min` must be <= Host core API;
  `core_api_max` is optional (omit / 0 = no upper bound, so older modules keep
  loading on newer Hosts)
- Undefined ELF symbols ⊆ `docs/module_sdk/host_export_allowlist.json`
- Link `-Wl,-z,max-page-size=4`
- No `#if CONFIG_*` for Host feature flags inside the plugin
- Start from the L3 template, not an existing vendor adapter

Load guard: `/store/modules/<id>/.load_guard`. Clear with
`POST /api/modules` `{"id":"...","clear_load_guard":true,"reload":true}`.
""",
    "js_api": """# L2 host.* API (from minihub_js.c — do not invent)

```
host.log.info(msg) / warn(msg) / error(msg)

host.point.register({ point_id, type?, adapter?, value?, online?, props? | params? })
  // point_id required; type defaults "sensor"; adapter defaults "js";
  // params is shorthand for { params: <obj> } inside props
host.point.get(point_id) → { point_id, type, value, adapter, online, props } | null
host.point.set(point_id, value, props?)      // throws if the point is unknown
host.point.unregister(point_id)
host.point.mergeProps(point_id, props)       // keeps other top-level props keys
host.point.mergeParams(point_id, params)     // writes props.params only

host.command.on(adapter_name, handler)
host.command.invoke({ point_id, cmd, params? })   // throws on failure

host.event.subscribe(name_or_star, handler)       // max 8 subscriptions
host.timer.setTimeout(fn, ms) / clearInterval / clearTimeout  // max 16 timers
host.adapter.poll(fn) / host.adapter.clearPoll()  // max 8, ~5 s tick, runs on the JS worker
host.time.offsetMin() / host.time.id()

host.http.request({ url, method?, headers?, body?, timeout_ms?, response_b64? })
  → { status, body }   // blocking ON THE JS worker; there is no URL allowlist
host.http.requestAsync(opts) → Promise            // max 4 in flight, I/O off the worker

host.config.get() → merged adapters JSON (or null)
host.config.save(root)

host.module.rpc(moduleId, method, params?)        // call another module's RPC
host.module.registerRpc(method, handler)          // only inside setup(host)
host.net.udp.open({ port }) / close(h) / sendto(h, {…}) / recvfrom(h, { timeout_ms, max_len })
host.crypto.md5 / sha1 / sha256 / hmacSha256 / aes128CbcEncrypt / aes128CbcDecrypt
```

All of this runs on one `js_worker` task shared by every L2 module: a blocking
`host.http.request`, a long poll callback or a slow RPC handler stalls the other
modules and their timers. Prefer `requestAsync`. Do not invent `host.*` names —
anything not listed above does not exist.
""",
    "pack": """# Pack and install

Outer package is zip `.mhub` (Host `hub_zip.c`). MHMD is the **inner** native image, not the zip format.

L1/L2 from this MCP: tool `pack_plugin` on a module directory.

In the firmware repo:

```
python3 tools/pack_store_module.py pack <id>
python3 tools/sync_store_catalog.py
python3 tools/store_server.py
```

Point Hub Store at the catalog (`custom_url`), refresh, install.
There is no documented multipart `file=@*.mhub` install unless the running firmware says so.

L3: build Host first so `application/minihub/build/modules/libadapter_<id>.so` / MHMD `.so` exists, then pack.
""",
    "constraints": """# Constraints

- Plugin ids and git branches: underscores, not hyphens
- Host product = `application/<name>/`; Core = `components/` (no vendor rules in Core)
- Public Host must not bake vendor cloud tokens or default vendor devices into `configs/` seeds
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

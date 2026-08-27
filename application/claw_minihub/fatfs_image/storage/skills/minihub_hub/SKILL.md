---
{
  "name": "minihub_hub",
  "description": "DEFAULT for 开灯/关灯/开插座/关插座/列出设备, 米家/miot, and MiniHub Store 插件 (可安装的插件/适配器 catalog). Use hub.api GET /api/store/catalog — NEVER skills_lab / ESP-Skill hub for 插件. MCP points.list + command.invoke for loaded adapters.",
  "metadata": {
    "cap_groups": [
      "cap_minihub_local_mcp"
    ],
    "manage_mode": "readonly"
  }
}
---

# MiniHub Hub Control

Use this skill by default when the user asks to list devices, read state, or toggle lights/switches/plugs — including plain **开灯 / 关灯** (no need to say 米家 or MiniHub).

All device actions MUST go through local MiniHub MCP (`mcp_call_tool` → `command.invoke` / `points.list`). Do not use Lua, GPIO, or board LED scripts for lights.

## MCP server (same device)

MiniHub tools run on the co-located `cap_mcp_server` instance. Prefer loopback:

| Field | Value |
| --- | --- |
| `server_url` | `http://127.0.0.1:18791` |
| `endpoint` | `mcp` (omit to use default) |

Do **not** use this board's LAN IP (`172.x` / `192.168.x` from `mcp_discover`) as `server_url` from the on-device agent — HTTP to our own Ethernet address does not hairpin and returns `ESP_ERR_INVALID_STATE`. Always `127.0.0.1`. Prefer `server_url` over `server_name`.

This Function board may run **Ethernet only** (Wi-Fi SoftAP skipped to save RAM). `wifi.status=disconnected` does **not** mean the hub is offline. If `wifi.status` is `ethernet` or `ip.connected` is true, MCP and Skill Hub work.

Alternative: call `mcp_discover` with `include_self: true` and then **rewrite** the returned URL host to `127.0.0.1` before `mcp_call_tool`. Default port is **18791**; endpoint path is **mcp**.

## Allowed tools

Use these remote tool names via `mcp_call_tool`:

| Tool | Purpose |
| --- | --- |
| `hub.api` | **Prefer for new Host Web features.** Allowlisted `/api/*` (method+path; optional query/body) |
| `points.list` | List points; optional `adapter` (loaded adapter id) or `type` filter |
| `points.get` | Read one point; requires `point_id` from a prior list |
| `points.history` | Event log shortcut (same as `hub.api` GET `/api/points/history`) |
| `points.set` | Set a scalar value; requires `point_id` and `value` |
| `command.invoke` | Run adapter command; requires `point_id`, `cmd`, optional `params_json` |
| `rules.list` | Read MiniHub automation rules (`when` point.updated / time) |
| `rules.put` | Replace and hot-reload MiniHub rules JSON (`rules_json` string) |
| `adapters.get` | Read saved adapters.json snippet |
| `hub.reload` | Soft-reload hub config/adapters (rate-limited) |
| `store.config.get` | Read saved MiniHub Store server URL |
| `store.config.set` | Save MiniHub Store base URL (`custom_url`) |

### `hub.api` (allowlist)

Do **not** invent paths outside the allowlist. Denied responses have `"error":"DENIED"`.

| method | path | notes |
| --- | --- | --- |
| GET | `/api/points` | query `type` / `adapter` |
| GET | `/api/points/history` | query `point_id` + `limit` (cloud+local) |
| GET | `/api/rules` | |
| GET | `/api/adapters` or `/api/config/adapters` | |
| PUT | `/api/config/adapters` | body `{"adapters":{...},"reload"?}` — enable/disable adapters (builtin or Store-loaded) |
| GET | `/api/system` | `local_ip`, `web_base` (for MiniHub login links) |
| GET | `/api/modules` | installed / loaded modules |
| GET | `/api/packs` | installed packs |
| GET | `/api/store/catalog` | **installable** Store items (id / name / version) |
| GET | `/api/store/config` | |
| POST | `/api/command` | body `{"point_id","cmd","params"?}` |
| POST | `/api/rules/import` | body `{"rules":[...]}` |
| POST | `/api/store/config` | body `custom_url` / `disable_default` |
| POST | `/api/modules/<id>/rpc` | body `{"method","params"}` — methods depend on **installed** module |
| GET | `/api/ota` | OTA status |
| POST | `/api/ota` | body `{"url":"https://..."}` URL OTA (not binary upload) |
| POST | `/api/hub/reload` | soft hub reload |
| POST | `/api/zigbee/permit_join` | body optional `{"seconds":60}` |
| POST | `/api/ble_mesh/permit_provision` | body optional `{"seconds":60}` |
| POST | `/api/store/install` | body `{"kind","id","reload"?}` |
| POST | `/api/store/upgrade` | body `{"kind","id","reload"?}` |
| POST | `/api/store/uninstall` | body `{"kind","id","reload"?}` |
| POST | `/api/store/refresh` | pull catalog from configured Store server |

Blocked examples: `/api/ota/upload` (binary). Prefer private chat for passwords.

**Builtin radio adapters** (e.g. zigbee when linked): disable via `PUT /api/config/adapters`. **Vendor clouds are optional Store plugins** — install only what the user needs; claw firmware does not require any one plugin.

### List installable Store plugins

**插件 ≠ Claw 技能.** Chinese **插件 / 可安装的插件 / Store 插件 / 适配器 / 米家插件** means **MiniHub Store modules** (`.mhub`), not ESP-Skill hub / `skills_lab_search` / `skills_lab_downloader`. Do **not** list flappybird / weather / games for these asks.

When the user asks what Store can install / which plugins are available / catalog / 列出可以安装的插件:

1. Activate this skill first.
2. Optional: `store.config.get` (confirm `custom_url`).
3. Optional: `hub.api` POST `/api/store/refresh` if they just changed the Store URL or say catalog is stale.
4. **Must** call `hub.api` GET `/api/store/catalog` — do **not** invent ids; do **not** stall without this call; do **not** substitute Skill hub results.
5. Reply with each item’s `id`, `name`, `version`, and `installed` true/false. Empty array → say catalog is empty / Store server unreachable; check URL + `store_server.py`.
6. To contrast **already installed**: GET `/api/modules` (and `/api/packs` if useful). Builtin `zigbee` may appear in modules even when not in Store catalog.

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"hub.api","arguments":{"method":"GET","path":"/api/store/catalog","query":"","body":""}}
```

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"hub.api","arguments":{"method":"POST","path":"/api/store/refresh","query":"","body":""}}
```

Install a module (replace `<id>` from catalog):

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"hub.api","arguments":{"method":"POST","path":"/api/store/install","body":"{\"kind\":\"module\",\"id\":\"<id>\",\"reload\":true}"}}
```

### Vendor login (miot / 米家) — prefer Web link

Do **not** ask for passwords in WeChat when a browser login link works. After installing `miot`, or when the user asks to 登录米家 / 登录 miot / bind Xiaomi account:

1. `hub.api` GET `/api/system` → read `local_ip` and `web_base` (claw: `/minihub`).
2. Build login URL with a **query** (WeChat auto-linkifies this; `#hash` often is not clickable):  
   `http://{local_ip}{web_base}?login=miot`  
   Example: `http://172.168.30.26/minihub?login=miot`
3. In the WeChat reply, put the URL on **its own line as plain text** — **never** wrap it in backticks `` ` ``, code fences, or Markdown link syntax. WeChat only makes bare `http://...` clickable.
4. Example reply shape:

```text
请用浏览器打开下面的链接登录米家（miot）账号：

http://172.168.30.26/minihub?login=miot

页面支持验证码/二次验证。登录成功后可选择家庭并同步设备。
```

5. Prefer this over collecting username/password in chat. Only fall back to `POST /api/modules/miot/rpc` `login` if the user insists.

After Store-install of `miot`, next-steps should include that plain URL line (not `` `http://...` ``).

Account / vendor RPC **after** that module is loaded (`POST /api/modules/<id>/rpc`). Lab serial: `module --list` / `module --rpc --id <id> --method …`. Do not invent methods — follow the module’s UI/docs or probe `status` first.

Magnet event log via gateway:

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"hub.api","arguments":{"method":"GET","path":"/api/points/history","query":"point_id=binary_sensor/miot_device_qgcg00&limit=20","body":""}}
```

Do **not** invent `point_id` values. Always call `points.list` first (optionally filtered) and use ids from the response.

## Recommended flow

1. Activate this skill (`minihub_hub`) when present. On claw_minihub, `mcp_call_tool` is also globally LLM-visible.
2. `mcp_list_tools` against `http://127.0.0.1:18791` to confirm MiniHub tools are present.
3. `mcp_call_tool` → `points.list` (add `adapter` when the user names a ecosystem).
4. For switches/lights/plugs: **always** `command.invoke` with `turn_on` or `turn_off`. Do **not** use `points.set` with `"on"`/`"off"` — that only updates Hub memory unless firmware maps it (still prefer `command.invoke`).
5. For brightness/color/CT, use `command.invoke` (`set_brightness`, `set_color`, `set_color_temp`) with `params_json` when supported.
6. For **事件记录 / 日志 / 何时打开** on a sensor (magnet, motion, button): `points.history` with that `point_id`. Items include `ts_ms`, `label` (e.g. 打开/关闭), `source` (`local` or `cloud`). Same data as Host Web「日志」. Do not invent timestamps.
7. After a successful write, optionally `points.get` and confirm the device state changed; report `INVOKE_FAILED` if the real device did not respond.

## Event log example (magnet)

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"points.history","arguments":{"point_id":"binary_sensor/miot_device_qgcg00","limit":"20"}}
```

Reply with `label` + time from `ts_ms` (epoch ms). Prefer cloud+local merged `items`; if `cloud_error` is set, still report any local rows.

## Automations (MiniHub rules — use events, never poll)

Sensor/light/plug linkage is **MiniHub rules**, not Claw `list_router_rules` / scheduler.

- Trigger is Hub `point.updated` (value change). Magnet uses `open` / `closed`.
- Persist with MCP `rules.put`. Do **not** invent polling, Lua sleep loops, or Claw IM-only rules for this.
- `list_router_rules` is Claw message routing (WeChat → agent). It cannot see MiniHub sensors.

When the user asks to link a magnet to lights/plugs:

1. `points.list` (`adapter`/`type` empty) to get real ids.
2. `rules.list` to keep unrelated rules you should not drop.
3. `rules.put` with `rules_json` as a **string** of `{"rules":[...]}`.
4. Prefer **one `then` object per rule** (most compatible). For multiple devices, use **multiple rules** with the same `when`, or after firmware supports it: `then` as an array of command objects (max 4).
5. Do **not** invent `then` as anything other than object or array of objects. Do **not** include PIR `switch/miot_2s_d50400`, `switch/demo`, or onboard LED.

Door open → light + real plug (four rules, one command each):

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"rules.put","arguments":{"rules_json":"{\"rules\":[{\"id\":\"magnet_open_light\",\"enabled\":true,\"when\":{\"type\":\"point\",\"point_id\":\"binary_sensor/miot_device_qgcg00\",\"equals\":\"open\"},\"if\":[],\"then\":{\"type\":\"command\",\"point_id\":\"light/miot_device_322801\",\"cmd\":\"turn_on\"}},{\"id\":\"magnet_open_plug\",\"enabled\":true,\"when\":{\"type\":\"point\",\"point_id\":\"binary_sensor/miot_device_qgcg00\",\"equals\":\"open\"},\"if\":[],\"then\":{\"type\":\"command\",\"point_id\":\"switch/miot_3_413196\",\"cmd\":\"turn_on\"}},{\"id\":\"magnet_close_light\",\"enabled\":true,\"when\":{\"type\":\"point\",\"point_id\":\"binary_sensor/miot_device_qgcg00\",\"equals\":\"closed\"},\"if\":[],\"then\":{\"type\":\"command\",\"point_id\":\"light/miot_device_322801\",\"cmd\":\"turn_off\"}},{\"id\":\"magnet_close_plug\",\"enabled\":true,\"when\":{\"type\":\"point\",\"point_id\":\"binary_sensor/miot_device_qgcg00\",\"equals\":\"closed\"},\"if\":[],\"then\":{\"type\":\"command\",\"point_id\":\"switch/miot_3_413196\",\"cmd\":\"turn_off\"}}]}"}}
```

Replace ids from `points.list`. `switch/miot_2s_d50400` is a **PIR**, not a plug — never control it in this scene.

After `rules.put`, say the scene is **active on events**. Do not ask to poll.

## mcp_call_tool shape (critical)

Top-level keys for `mcp_call_tool` — **not** nested under `arguments`:

| Key | Required | Example |
| --- | --- | --- |
| `server_url` | yes | `http://127.0.0.1:18791` |
| `tool_name` | yes | `points.list` |
| `arguments` | yes (object) | `{"adapter":"","type":""}` |

**Wrong:** `"server_name"` only (without `server_url`) used to fail with `ESP_ERR_INVALID_ARG` before MCP ran. Prefer `server_url`. Do **not** put `server_url` / `tool_name` inside `arguments`.

## Tool call examples

List all points (engine requires the filter keys; empty means no filter):

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"points.list","arguments":{"adapter":"","type":""}}
```

List points for one loaded adapter (example id — use a real adapter from `adapters.get`):

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"points.list","arguments":{"adapter":"<adapter_id>"}}
```

Turn on a light (use a real `point_id` from step 3):

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"command.invoke","arguments":{"point_id":"light/miot_device_322801","cmd":"turn_on"}}
```

Turn off a switch:

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"command.invoke","arguments":{"point_id":"<id>","cmd":"turn_off"}}
```

Save a custom MiniHub Store URL (WeChat: 「把 Store 地址设为 http://HOST:PORT」):

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"store.config.set","arguments":{"custom_url":"http://192.168.1.10:8080","disable_default":"false"}}
```

Read it back:

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"store.config.get","arguments":{}}
```

`custom_url` must be `http://` or `https://` with host (optional `:port`). Empty `custom_url` clears the override. When setting a URL and the user wants only that server, pass `"disable_default":"true"`.

This Function image **does** have LittleFS `modstore` mounted at `/store` (2MB). Catalog download and `.mhub` install can work **if** the store server implements MiniHub Store APIs.

## Store vs website (do not confuse)

The hub fetches **`GET {custom_url}/api/catalog`** and expects a **JSON array**. It does not open an HTML homepage.

If `http_request` to `http://HOST:PORT/` or `/api/catalog` returns **404 on every path**, that host is a static website (or empty nginx), **not** `tools/store_server.py`. Tell the user to run:

```text
python3 tools/store_server.py --host 0.0.0.0 --port <PORT> --deploy ~/workspace/minihub_store
```

Do **not** conclude “this firmware cannot install adapters” from a remote 404. The missing piece is the store server, not the `/store` partition.

`ESP_ERR_HTTP_CONNECT` to `http://172.168.10.13:8080` is the firmware **default** store (unreachable on this LAN). Ignore it when `disable_default` is true.

## Adapters (`adapters.get`)

`adapters.get` returns **saved `adapters.json`**, or the firmware default if `/data/config` is missing (Claw has no LittleFS `/data`; config is in-RAM defaults).

Firmware default may include Zigbee (static MVP) with `"enabled": false`. That does **not** mean install failed. **Vendor adapters appear only after their Store module is installed** (and often after login/sync).

Workflow:

1. List modules / adapters: `hub.api` GET `/api/modules` or ask user for serial `module --list`.
2. If the needed cloud is missing → Store-install that `<id>`.
3. Bind via `POST /api/modules/<id>/rpc` with that module’s methods (often `login` / `status` / `sync`).
4. `points.list` with `adapter` filter from real loaded adapters — never assume a fixed vendor name.

| Situation | What to do |
| --- | --- |
| module missing | `POST /api/store/install` with the user’s chosen id |
| logged out / no devices | **Prefer** plain URL `http://{ip}/minihub?login=<id>` on its own line (from GET `/api/system`); else module login/sync RPC |
| devices present, `points.list` empty | `hub.reload`, wait ~10s, list again |
| user wants to remove a Store module | `store/uninstall` for that id |

Do **not** require any specific plugin (including `miot`) for claw to be useful.

## Store install (do not stall on this)

`hub.reload` does **not** install modules. It only reloads already-registered adapters/config.

A leftover `/store/modules/<id>.staging/<id>.mhub` file is **not** an install. Real install downloads the zip into RAM, **extracts** `module.json` + `.so` into `<id>.staging/`, then **renames** that directory to `/store/modules/<id>/`. Never copy the zip into staging and wait.

Prefer `hub.api` for Store install (no separate `store.install` tool). The remote Store server only serves `/api/catalog` and files; the hub is the installer:

```json
{"server_url":"http://127.0.0.1:18791","tool_name":"hub.api","arguments":{"method":"POST","path":"/api/store/install","body":"{\"kind\":\"module\",\"id\":\"<id>\",\"reload\":true}"}}
```

Do **not** say you are waiting to confirm the activation channel.

`store/uninstall` applies to Store-installed modules. Builtin radio adapters (if any) are disabled via `PUT /api/config/adapters`, not Store uninstall.

Claw may run with **zero** cloud plugins. Only Store-install what the user asks for.

## Errors

| Response | Action |
| --- | --- |
| `HUB_NOT_READY` | Wait ~30s and retry. Do **not** call `restart_device` or busy-wait. |
| `ESP_ERR_INVALID_STATE` on MCP HTTP | Retry `server_url` `http://127.0.0.1:18791`. |
| Empty `points.list` | Confirm which adapters/modules are loaded; install/login/sync as needed. |
| `module not ready` / RPC fail | Module not loaded — Store-install + reload. |
| `INVOKE_FAILED` / `SET_FAILED` | Report error; re-list points if ids may be stale. |

If `mcp_list_tools` returns no `points.*` tools, stop and tell the user MiniHub MCP is not registered yet.

## Rules

- Never bypass MCP to control hub devices.
- Never fabricate `point_id`, adapter names, or device capabilities.
- Prefer concise replies with the ids and states the user asked for.
- One list → act → confirm cycle per user request unless they ask for multiple devices explicitly.
- When the user gives a MiniHub Store URL, call `store.config.set` then `store.config.get`.
- When asked to 登录米家 / login miot (or after installing `miot`), call GET `/api/system` and reply with a **bare** URL on its own line: `http://{local_ip}{web_base}?login=miot` (no backticks). Prefer that over collecting passwords in WeChat.
- If Store catalog returns 404, say the **server** is not MiniHub Store (`/api/catalog` JSON).
- Do not require a specific vendor plugin; install only what the user needs.
- Never call `hub.reload` to “activate” a package without install. Never call `restart_device`. Never write a Lua busy-wait loop.
- `points.list` with `arguments: {}` fails (`Missing argument: adapter`). Always pass `"adapter"` and `"type"` strings (empty to list all).

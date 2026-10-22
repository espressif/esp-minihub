---
name: minihub-plugin-l2
description: Create or edit MiniHub Store L2 JavaScript plugins (module.json + main.js, host.* APIs). Use when the user wants JS automation, QuickJS plugins, or L2 Store modules.
---

# MiniHub Store L2 (JS)

L2 is a **Store packaging tier** (JS module → `.mhub`), not a Host Core layer. Needs `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS` on the Host.

## Where this fits

| Layer | What belongs there |
|-------|--------------------|
| Host Core | Type-agnostic points, web, device-widget catalog |
| Bundled Host adapters | Radio / protocol stacks via Kconfig |
| Store L2 | QuickJS automation / glue via `host.*` — no `esp_wifi_*` / radio from JS |

## Where sources live

| Kind | Location |
|------|----------|
| Real Store JS module | sibling repo via `MINIHUB_PLUGIN_DIRS`, or `examples/plugins/<id>/` (scaffold default) |
| Standalone demos | `examples/plugins/<id>/` + `MINIHUB_PLUGIN_DIRS` |

Do **not** put demos into Host `application/*/components` or default `store_preinstall` (public product list is empty).

## Scaffold

```bash
python3 tools/new_store_module.py --tier l2 --id my_js --name "My JS"
```

Or IDE MCP: `scaffold_plugin` with `tier=l2`. Do not invent `host.*` fields; ask `plugin_guide` topic `js_api`.

## Files

- `module.json`: `"runtime": "js"`, `type`: `"module"`
- `main.js`: QuickJS; Host bindings in `components/minihub_js/minihub_js.c`
- Optional `meta.json` / `web/`

## Data model (when touching points)

- Core is type-agnostic: one device = one `minihub_point_t`; live values in `props.params.<name>`.
- UI widgets come from Host device-widget catalog (`toggle` / `slider` / `dropdown` / `text` / `hidden`), not from inventing C entity structs.

## JS Host API (do not invent)

- `host.log.info/warn/error(msg)`
- `host.point.register({ point_id, type, adapter, value, online, props|params })`
- `host.point.get` / `host.point.set(id, value, props?)`
- `host.command.on(adapter, handler)` — route Core commands into JS
- `host.command.invoke({ point_id, cmd, params? })`
- `host.adapter.poll(fn)` — ~5 s adapter tick (max 8 handlers); `host.adapter.clearPoll()` drops every handler in the runtime
- HTTP helpers if present in the same file — match existing names, do not guess

Do not call native Wi-Fi / radio APIs from JS.

Devices-page light demo: `examples/plugins/color_bulb_js/`.

## Range scaling (Hub % ↔ device raw)

When a parameter maps the Hub's **1–100** percentage UI onto a device-native
range with a different max (e.g. Yeelight brightness `1–65535`, read from the
spec `value-range` or `profiles.json` as `bright_max`), the scaling MUST follow
this rule:

- **Pin the endpoints**: `1% → device minimum (1)`, `100% → device maximum
  (bright_max)`. The extremes are **NOT** scaled.
- **Spread the middle**: `2%–99%` are distributed evenly across the remaining
  `[1, maxv]` range (linear interpolation with both ends pinned).

Formulas — identical in the C adapter and the JS plugin, keep them in sync:

```text
raw = 1 + round((pct - 1) * (maxv - 1) / 99)     # pct→raw, clamp [1, maxv]
pct = 1 + round((raw - 1) * 99 / (maxv - 1))      # raw→pct, clamp [1, 100]
```

Only scale when `maxv > 100`; for `maxv <= 100` (e.g. the c500ch camera,
`bright_max=100`) pass the value through unchanged.

This rule applies to **every** parameter that needs percentage→native scaling
(not just brightness), and to **both** L2 JS plugins (`miot_js/main.js`
`coerceValue` / `toHubValue` `bright` branches) and L3 native/C adapters
(`miot/miot_adapter.c` `bright_pct_to_raw` / `bright_raw_to_pct`). The two
implementations must stay bit-for-bit equivalent.

Reference + regression test: `miot/tools/check_bright_scale.py`
(run `python3 miot/tools/check_bright_scale.py`, no host deps).

## Pitfall: the JS worker is single-threaded — never run a long blocking job

All L2 modules share ONE QuickJS context (`s_ctx` in `minihub_js.c`); the worker
executes jobs one at a time. A single `job->fn` that runs a long synchronous
function monopolizes the worker, so every other module RPC (device detail /
control / rules) queues until it finishes.

### Root cause of the miot sync "stuck syncing" bug (Athena #8b68c6c5, weblink 22x6Aswm)

`miot_js`'s `syncDevices` was one giant synchronous function: it chained several
blocking `host.http.request` → `esp_http_client_perform`, plus `miioDiscover(2000)`,
per-device `miioCall`, and `cfgSave`, all inside a single job. While it ran, the
worker was held for the whole sync (tens of seconds to minutes), so:

- Clicking a device / adding a rule mid-sync did nothing; the page stayed on
  "syncing" even after the user stopped clicking.
- The web "Sync" button is easy to mash; each click enqueued another full sync,
  stacking minutes-long jobs → looked permanently stuck, then fired a burst of
  "已发送" toasts when they all drained.

The `sync` RPC had already been made async (it moved the block from the httpd
task into the worker), but the worker itself was still the bottleneck — the
single long job never yielded.

### Fix pattern (two layers)

1. **Drop redundant sync (Layer 1, web side).** In `syncModule`, guard with
   `authModuleSyncing(mid)`: if a sync for that module is already in flight,
   `toast(ov.sync_busy)` and return — do NOT enqueue a second one. Scoped per
   module id (`syncingModuleIds[mid]` in `app.js`); different modules do not
   block each other. (Optionally also reject `sync` on the host side with
   `sync_in_progress` when one is already flying.)
2. **Make the sync cooperative (Layer 2, plugin side — the real fix).** Split
   `syncDevices` into a staged state machine. The `sync` RPC returns
   `{sync:"pending", syncing:true}` immediately and drives the phases on a
   `setTimeout` chain (`device_list` → `gethome` → `discover` → per-device →
   `register` → `save`). Between phases the worker loop returns to the top
   (`js_async_drain` / `js_timers_fire_due`) and services queued device RPCs, so
   a click mid-sync gets a ~1–2s response instead of tens of seconds. `status()`
   reports `syncing:true` until the run finishes; the web already polls
   `status()` for `pending/syncing` and waits, so no extra web change is needed.

### Constraints / gotchas

- You **cannot** give sync its own thread: `s_ctx` is a shared singleton, so JS
  must yield cooperatively inside the same worker.
- `host.http.request` is still **blocking** at the C layer for synchronous callers.
  The plugin-facing `host.http.requestAsync` (returns a Promise) exists so a caller
  can `await` without blocking the worker — but it only resumes because the worker
  loop drains QuickJS pending jobs (`js_jobs_drain`); see the job-queue pitfall
  below. Each phase therefore still blocks for ~one HTTP round-trip at the C layer;
  the cooperative split only shrinks the monopoly from "whole sync" to "single phase".
- Validate any such change on a real device (DUT) before merging — the worker is
  shared by all L2 modules, so a hang here freezes every JS plugin.

### Pitfall: `await` / Promises only resume if the worker drains the job queue

`host.http.requestAsync` returns a real QuickJS Promise. quickjs-ng queues every
`.then`/`.catch` continuation and async-function resume into `rt->job_list`; the
ONLY consumer is `JS_ExecutePendingJob`, which the worker loop calls every
iteration via `js_jobs_drain()` in `minihub_js.c`. Node auto-drains microtasks, so
this cannot be reproduced in a Node harness — but on-device, if the drain is
removed or skipped, an awaited call never resumes: the code after `await` (e.g. a
`finally { running = false }` guard reset) never runs, so the next periodic poll
early-returns forever and cloud values silently stop updating.

Rules:
- Any `async`/`await` or `.then`/`.catch` in an L2 module depends on the worker
  draining pending jobs. Do not assume it "just works" like in Node.
- The Host MUST keep `js_jobs_drain()` (the `JS_ExecutePendingJob` loop) in the
  worker loop. If you refactor the loop, preserve it.
- Validate async paths on a real device (DUT): after a sync, confirm device
  values keep updating, or that the poll path logs a second refresh.
- The drain loop MUST be **bounded** (cap jobs per worker iteration, e.g. 64). A
  self-resolving Promise chain (or any code that re-queues jobs) would otherwise
  loop forever inside one `js_jobs_drain()` call and wedge the worker — the same
  freeze the cooperative split was meant to prevent. Capping bounds ONE pass, so
  `js_jobs_drain()` MUST report that the cap was hit and the worker MUST then
  shorten its wait (1 tick) instead of blocking: a leftover job generates no
  notify of its own, and with no timer armed the wait is `portMAX_DELAY` — which
  strands those reactions exactly like a missing drain.
- Any Host async API that holds `JSValue`s across a task boundary (e.g.
  `host.http.requestAsync` keeping `resolve`/`reject` while a separate task does
  blocking I/O) MUST register its in-flight requests and **reject + drop them on
  module unload**, keyed to the owning `JSRuntime`. Otherwise a Store
  install/upgrade/uninstall mid-flight frees `s_ctx`/`s_rt` while the task is
  still running: the completion callback then feeds a freed/null context into
  QuickJS (crash) or calls `JS_Call` on JSValues from the old runtime
  (use-after-free). Mirror the in-flight registry pattern used by timers/polls:
  list the requests, set a `cancelled` flag on unload, reject the Promise with
  the still-valid context, `JS_FreeValue` the held JSValues, and let the task
  free its own struct (never free it from unload, or you double-free).

## Env

| Variable | Default | Meaning |
|----------|---------|---------|
| `MINIHUB_APP` | `minihub` | Product under `application/` |
| `MINIHUB_PLUGINS_DEPLOY_DIR` | `~/workspace/minihub_store` | `Stores/Common/<id>/` |
| `MINIHUB_PLUGIN_DIRS` | (empty) | Extra source trees (`:`/`;`). Set if the module is under `examples/plugins/` |

No native compile. Packer zips `module.json` + `main.js` (+ `meta.json` / `web/` if present). Host must already have `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS`.

## Pack (zip → `.mhub`)

```bash
# optional if sources are under examples/plugins/<id>/
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"

python3 tools/pack_store_module.py pack my_js
python3 tools/sync_store_catalog.py
python3 tools/store_server.py --port 8080
```

IDE MCP `pack_plugin` can zip L2 the same way (no `.so`).

## Artifacts

```text
$MINIHUB_PLUGINS_DEPLOY_DIR/catalog.json
$MINIHUB_PLUGINS_DEPLOY_DIR/my_js/my_js.mhub   # zip: module.json + main.js [+ ui/web]
```

## Install

- **Store (usual):** Hub Store `custom_url` → refresh → install → `/store/modules/<id>/`.
- **Factory preinstall (optional):** app `manifest.yml` `store_preinstall: [my_js]` + rebuild/flash. Public Host keeps `[]`.

Docs: `docs/en/plugins/develop_l2.rst` and `docs/en/api/js_host.rst`; Host bindings in `minihub_js.c`. See also `tools/README.md`.

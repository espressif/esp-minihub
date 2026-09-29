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

- `module.json`: `"runtime": "js"`, `kind`: `"module"`
- `main.js`: QuickJS; Host bindings in `components/minihub_js/minihub_js.c`
- Optional `ui.json` / `web/`

## Data model (when touching points)

- Core is type-agnostic: one device = one `minihub_point_t`; live values in `props.params.<name>`.
- UI widgets come from Host device-widget catalog (`toggle` / `slider` / `dropdown` / `text` / `hidden`), not from inventing C entity structs.

## JS Host API (do not invent)

- `host.log.info/warn/error(msg)`
- `host.point.register({ point_id, type, adapter, value, online, props|params })`
- `host.point.get` / `host.point.set(id, value, props?)`
- `host.command.on(adapter, handler)` — route Core commands into JS
- `host.command.invoke({ point_id, cmd, params? })`
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

## Env

| Variable | Default | Meaning |
|----------|---------|---------|
| `MINIHUB_APP` | `minihub` | Product under `application/` |
| `MINIHUB_PLUGINS_DEPLOY_DIR` | `~/workspace/minihub_store` | Catalog + `.mhub` |
| `MINIHUB_PLUGIN_DIRS` | (empty) | Extra source trees (`:`/`;`). Set if the module is under `examples/plugins/` |

No native compile. Packer zips `module.json` + `main.js` (+ `ui.json` / `web/` if present). Host must already have `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS`.

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

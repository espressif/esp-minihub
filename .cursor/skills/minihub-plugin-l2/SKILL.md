---
name: minihub-plugin-l2
description: Create or edit MiniHub Store L2 JavaScript plugins (module.json + main.js, host.* APIs). Use when the user wants JS automation, QuickJS plugins, or L2 Store modules.
---

# MiniHub Store L2 (JS)

Needs `CONFIG_MINIHUB_DYNAMIC_MODULES` and `CONFIG_MINIHUB_JS_PLUGINS` on the Host.

## Scaffold

```bash
python3 tools/new_store_module.py --tier l2 --id my_js --name "My JS"
```

## Files

- `module.json`: `"runtime": "js"`, `kind`: `"module"`
- `main.js`: QuickJS; Host bindings in `components/minihub_js/minihub_js.c`
- Optional `ui.json` / `web/`

## JS Host API (do not invent)

- `host.log.info/warn/error(msg)`
- `host.point.get/set` as implemented in `minihub_js.c`
- `host.command.invoke({ point_id, cmd })` — **no `params` in JS today**
- HTTP helpers if present in the same file — match existing names, do not guess

Do not call native Wi-Fi / radio APIs from JS.

## Pack

```bash
python3 tools/pack_store_module.py pack my_js
python3 tools/sync_store_catalog.py all
```

Docs: `docs/en/plugins/develop_l2.rst` and `docs/en/api/js_host.rst` when present; else `docs/plugins_and_store.md` + `minihub_js.c`.

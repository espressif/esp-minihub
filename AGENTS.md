# Agent instructions (MiniHub)

For any AI coding assistant. Prefer this file and `docs/` over IDE-only config when they conflict.

After every feature or bugfix, follow `.agent/private/HANDOFF.md` (new `HANDOFF_年月日_时分秒.md`, or append when that feature/bug already has one).

`.agent/private/README.md` is the shared artifact contract for **every** agent working here (Cursor / CodeBuddy / Qwen / Qoder): naming, append-only lifecycle, provenance labels (实测 / 静态 / 猜测), what is tracked, and the secret boundary. Tool-private dirs hold pointers to it, never copies.

`.agent/` holds exactly three dirs: `skills/` and `mcp/` are committed, `private/` is handoffs / plans / reviews / memory / scratch and is **not** committed. `.cursor/skills` and each tool's `mcp.json` are symlinks into `.agent/` — edit the `.agent/` copy, never a duplicate. Read the matching **project skill** under `.agent/skills/` before editing Host, Store plugins, or docs.

## Docs map (use what exists)

Customer programming guide is RST / esp-docs: [docs/en/](docs/en/index.rst) · [docs/zh_CN/](docs/zh_CN/index.rst). Build with `./tools/build_docs.sh` or `cd docs && build-docs -bs html -t esp32s31 -l en` (output: `docs/_build/`; see [docs/README.md](docs/README.md)).

| Topic | RST (preferred) |
|-------|-----------------|
| Build / Host product | `docs/en/getting_started.rst` · `docs/en/host_customization.rst` |
| Plugins / Store | `docs/en/plugins/` · allowlist `docs/module_sdk/` · `examples/plugins/` |
| Data model / type catalog | `docs/en/data_model.rst` · `docs/en/architecture_overview.rst` · `components/minihub_device_widget/README.md` |
| REST / JS / MCP / C ABI | `docs/en/api/` |
| Networking / Matter | `docs/en/networking.rst` · `docs/en/matter.rst` (upstream: [ESP-IDF docs](https://github.com/espressif/esp-idf/tree/master/docs), [ESP-Matter docs](https://github.com/espressif/esp-matter/tree/main/docs)) |
| Production | `docs/en/production.rst` |

Chinese mirrors: `docs/zh_CN/` (same relative paths as `docs/en/`).

End-user / lab notes belong under the app only when present — **not** as Host Core docs.

## Git

- No tool attribution: no `Co-authored-by: Cursor`, no `Made-with: Cursor`.
- Conventional commits; author is `user.name` / `user.email`.
- Branch names: **underscores only** (no hyphens), e.g. `feature/modbus_tcp_client`.

## Vibe Coding tasks

L1 / L2 / L3 are **Store packaging tiers** (`.mhub`), not Host Core layers. Host = Core + bundled adapters; demos = `examples/plugins/` + `MINIHUB_PLUGIN_DIRS` (default `store_preinstall: []`).

| User wants | Skill | Do not |
|------------|--------|--------|
| New/OEM Host product, Kconfig, seeds, stacks | `.agent/skills/minihub-host/` | Bake vendor cloud tokens / demos into Core or Host `require` |
| L1 pack (inject into an existing adapter) | `.agent/skills/minihub-plugin-l1/` | Add a new radio stack; treat L1 as Host Core |
| L2 JS plugin (`main.js`) | `.agent/skills/minihub-plugin-l2/` | Call `esp_wifi_*` from JS; put demos in Host builtin |
| L3 native `.so` | `.agent/skills/minihub-plugin-l3/` | `#if CONFIG_*` Host options; invent per-type C structs; default-preinstall demos |
| Programming guide RST/MD | `.agent/skills/minihub-docs/` | Dual-write RST and old MD as two sources of truth |

Scaffold a plugin:

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native
```

Templates: `docs/module_sdk/templates/`.

IDE MCP for plugin authors (Cursor / VS Code, **not** the Hub on-device MCP):

```bash
python3 tools/minihub_plugin_mcp.py
```

Repo configs live in `.agent/mcp/` (`cursor.mcp.json`, `vscode.mcp.json`), symlinked to `.cursor/mcp.json` and `.vscode/mcp.json`. Tools: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin`.

## Host constraints

- Product = `application/<name>/`. Default public Host: `application/minihub`.
- Shared Core: `components/minihub_*`, bundled Hub adapters under `components/minihub_adapters/`. Store plugin sources: `examples/plugins/<id>/` (in-tree demos) or a sibling repo via `MINIHUB_PLUGIN_DIRS`. Products include `cmake/minihub_bundle/minihub_import.cmake` then `minihub_register_kconfig()`; or one yml path `cmake/minihub_bundle`. Do not put ecosystem-specific rules in Core.
- Vendor / demo adapters ship as Store `.mhub`, not Host `require`. Public Host `store_preinstall` stays empty unless intentionally bundling factory modules.
- ESP-IDF **~6.2** + target **esp32s31** (`idf.py --preview`). IDF 5.x has no S31 toolchain.
- Optional **on-device** MCP: `CONFIG_MINIHUB_MCP_SERVER=y` + `git submodule update --init components/esp-claw`. Unrelated to the plugin-author IDE MCP above.

## Core data model (design)

- MiniHub Core is **type-agnostic**: every device is **one** generic
  `minihub_point_t` (point_id / `type` string / `value` string / `props` cJSON* /
  adapter / online). Core does NOT define a C `struct` per device type
  (light / switch / …) — plugins are runtime `.so`, so types are not
  known at compile time.
- **One device = one point.** `point_id` is `<protocol>.<device>` (e.g.
  `sim.color_bulb` or `thread.color_bulb`). Live parameters, access, visibility,
  and instance bounds live in structured JSON on that point — typically
  `props.params.<name> = { "value", optional access / visible / min / max /
  step / unit }`. `value` is an optional compact summary (e.g. power on/off)
  for overview cards and history; it is not one point per parameter.
  Overview **能力点** counts collapsed devices (one card per device), never
  one card per parameter. Do not ship stale `build/<id>.so` / deploy `.mhub`
  that still register `thread.<device>.<param>` multi-points.
- Device **type** is described declaratively, not in C:
  - Type-level UI catalog: `components/minihub_device_widget/model/*.csv` → generated
    registry (single source of truth for labels / widget / bounds / icons). It
    has **no effective** `visible` column (a leftover CSV header is tolerated and
    ignored) and **no** per-device override column: the catalog keyed by
    parameter NAME describes how a parameter is *rendered*, and offers every
    parameter of the type. Adding a type needs no C change.
  - Instance-level descriptor: a plugin's `devices.json` (embedded into
    `device_desc.gen.h`) lists THIS device's params, init values, and optional
    `access` / `visible` / `min` / `max` / `step` / `unit`; those fill
    `props.params` at register time. Catalog style supplies **defaults** (label /
    widget / bounds / css); instance keys **override** when present — do not
    copy the whole style blob. `visible:false` = the param is **still present in
    props but hidden in the UI** (internal use); `access:"R"` = the UI shows its
    value but offers no control. Instance `visible` is the **only** place
    visibility is decided — the UI intersects the type catalog with the live
    device point's params.
- No per-type C struct. If server-side validation/coercion is ever needed, register a
  handler table keyed by `type` string (function pointers), not a struct.
- The type catalog is **protocol-agnostic**: device display (label / widget / icon) is
  protocol-independent, so `devices.csv` and `minihub_device_widget_*_t` carry NO
  `protocol` field. The transport prefix (thread / zigbee / …) comes from the **device
  registration**: a registered point id is `<protocol>.<device>` and carries
  `type` = the data-model type, so `minihub_point_resolve_by_type_param(type, parameter)`
  resolves to the device point when `props.params` contains that parameter —
  e.g. ("light", "power") -> "sim.color_bulb". Control APIs pass `parameter` in
  the command so the adapter updates the matching key inside `props`. Never
  hardcode "thread".
- Bounds (min/max/step/unit): type catalog = **defaults**; instance `devices.json`
  may override per param when those keys are present. Prefer omitting them so the
  catalog stays the single maintained range table.

## Do not read unless needed

- `application/minihub/managed_components/`, `application/minihub/build/`
- `application/minihub/components/esp-rainmaker/` (upstream)
- `docs/_build/`, `.venv-docs/`

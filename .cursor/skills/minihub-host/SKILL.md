---
name: minihub-host
description: Develop MiniHub Host firmware products (application folder, Kconfig, JSON seeds, radio stacks, OEM copy). Use when the user changes Host, app_main, partitions, sdkconfig features, adapters.json, or copies a new product from minihub; also when the console web assets do not update after app-flash, a serial port is busy during a flash, a Matter-stub build is needed, or web console sessions appear to evict each other.
---

# MiniHub Host

## Read first

- `AGENTS.md`
- `docs/en/getting_started.rst` + `docs/en/host_customization.rst`

## Product layout

- Product = `application/<app>/` (`-app` / `MINIHUB_APP`).
- Shared Core = `components/minihub_*` and bundled adapters in `components/minihub_adapters/`. Product import: `cmake/minihub_bundle/minihub_import.cmake` + `minihub_register_kconfig()`. One-yml: `path: …/cmake/minihub_bundle`.
- Seeds: `application/<app>/config/{adapters,rules,scenes}.json` else repo `config/`.
- Manifest: `application/<app>/manifest.yml` extends `config/minihub_manifest.yml` `_host_base`.
- Device UI catalog: `components/minihub_device_widget/model/*.csv` (closed widgets: toggle / slider / dropdown / text / hidden) → runtime `/data/config/devices.csv`. Core stays type-agnostic (`minihub_point_t`; one device = one point, params in `props.params`).

## Host vs Store (do not confuse)

| In Host | Via Store `.mhub` |
|---------|-------------------|
| Core, Web, device-widget catalog | L1 pack / L2 JS / L3 native modules |
| Bundled adapters (Zigbee / BLE Mesh / Matter / MQTT / Modbus, …) | Vendor / demo plugins |

- Do **not** add vendor cloud adapters or demo plugins to Host `require`.
- Public `application/minihub` uses `store_preinstall: []`. Optional factory ids resolve under `examples/plugins/<id>/` **or** `MINIHUB_PLUGIN_DIRS`.
- Standalone demos live in `examples/plugins/` — not Host builtin.

## Plugins vs Host build

| Goal | What to do |
|------|------------|
| Pull extra sources into configure | `export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"` **before** `./tools/build_firmware.py … build` (`:`/`;` separated). Read at configure time only. |
| Ship `.mhub` to LAN Store | After L3 Host build (or L1/L2 pack): `$MINIHUB_PLUGINS_DEPLOY_DIR` (default `~/workspace/minihub_store`) has `catalog.json` + `<id>/<id>.mhub`. Serve with `python3 tools/store_server.py`. |
| Rebuild all L3 `.so` | `./tools/build_firmware.py modules` |
| Rebuild one L3 `.so` | `./tools/build_firmware.py module <id>` |
| Re-wrap existing L3 ELF | `./tools/build_firmware.py pack [all\|<id>]` |
| Factory-flash a module | App `manifest.yml` `store_preinstall: [<id>]` then rebuild/flash. Does not replace Store install. Public product stays `[]`. |

L1/L2: no Host link step — `python3 tools/pack_store_module.py pack <id>` then `sync_store_catalog.py`. L3: `./tools/build_firmware.py module <id>` produces `application/<app>/build/modules/libadapter_<id>.so`; `./tools/build_firmware.py pack <id>` wraps MHMD `build/<id>.so` into `.mhub`. Details: `.cursor/skills/minihub-plugin-l{1,2,3}/`.

## Workflow

1. Copy `application/minihub` → `application/<new>` only if creating a new product.
2. Edit **app** `manifest.yml` / `config/sdkconfig/app.yml` / JSON seeds. Touch `_host_base` only for all products.
3. `MINIHUB_APP=<app> python3 tools/sync_host_manifest.py`
4. Build (IDF ~6.2, `esp32s31`):

```bash
export IDF_PATH=/path/to/esp-idf-v6.2
# source "$IDF_PATH/export.sh"
./tools/build_firmware.py -app minihub zigbee ble_mesh build
```

`thread` and `zigbee` cannot be combined. Matter needs `ESP_MATTER_PATH`.

## Bench traps

| Symptom | What is actually going on |
|---------|---------------------------|
| Edited `components/minihub_web/web/**`, ran `app-flash`, browser still shows the old console | The console's JS/CSS ships in the **`modstore`** littlefs partition (`application/minihub/main/CMakeLists.txt`, `littlefs_create_partition_image(modstore …)`), and `app-flash` writes only the app. Flash `modstore-flash`, or hot-push one file: `curl -X PUT http://<hub>/api/dev/web/js/<file>.js --data-binary @components/minihub_web/web/js/<file>.js`. Prove which build is live from `window.mhWebBuild` in `web/js/boot.js` — bump that marker with every JS change, and hard-refresh after. |
| Two console clients keep logging each other out, or an auth test says the opposite of the code | Web auth holds **one** session slot (`s_sess_hex` in `components/minihub_auth/minihub_auth.c`): the newest login invalidates every other cookie. `Authorization: Bearer <CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN>` bypasses sessions entirely, so a curl+Bearer probe never observes eviction. Idle 300 s slides on document loads and writes only; absolute cap 8 h. |
| Need a build where `minihub_matter` compiles `matter_stub.c` while `CONFIG_MINIHUB_MATTER_ADAPTER=y` | `build_firmware.py` cannot produce it: `ensure_matter_stack()` re-adds the `matter` stack whenever that Kconfig is on, and `MINIHUB_BUILD_MATTER` is derived from the stack list, so a CLI/`env` value is overwritten. Run `idf.py --preview build` directly with `MINIHUB_BUILD_MATTER=0` and no `ESP_MATTER_PATH` (what CI does) — that is also the only configuration that links `matter_stub.c`, so every symbol callers outside the component use must exist there. |
| Reporting a build or test result | `cmd > log 2>&1; tail log` reports `tail`'s status, and a trailing `grep` returns 1 when it matches nothing — both look like the build's verdict. Use `cmd > log 2>&1; rc=$?; … ; exit $rc` and quote the recorded `rc`. |
| Console assets look unrefreshed after a flash | Console JS/CSS lives in `modstore`, device runtime state lives in `storage`: `./tools/build_firmware.py … flash` (no scope) writes `storage.bin` too and **wipes `/data`** — `adapters.json`, commissioned Matter rows, everything. Use `app-flash` / `modstore-flash` to keep `/data`; an empty point list plus a seed-shaped `adapters.json` right after a full flash is that wipe, not a regression. |
| Trying to measure cold vs cached asset load in the browser | A `/web/...js?cb=1` cache-buster **404s** (the handler matches the full URI, query included), and an in-page `fetch(..., {cache:'default'})` "warm" pass is a memory-cache hit, not an `If-None-Match` round-trip. Measure with curl and an explicit `If-None-Match` header; read the browser truth from `performance.getEntriesByType('resource')` — `transferSize` ~300 B with a full `encodedBodySize` is a real 304. |
| Run any subcommand with `matter` in the stacks and it aborts at once: `ESP_MATTER_PATH not found: …` | `tools/build_firmware.py` (`matter_env_exports`) hardcodes a default Matter checkout under `../esp-idf/mesh_lite_*/examples/matter_bridge/root/esp-matter`, and the single call site that extends the command shell with it is not inside the `build` branch — so `build`, `flash` and `dev_sync` all die there when the tree is gone. It is an environment error, not a code error. `export ESP_MATTER_PATH=<your esp-matter checkout>` (the one with `export.sh` + `connectedhomeip/connectedhomeip`) first. |
| `Could not open …/cu.usbserial-…: Resource busy` | The human's serial monitor owns the port. `lsof /dev/cu.usbserial-110` before flashing; ask, never kill it, and never leave your own `serlog`/monitor capture holding a port someone else needs. |

## MCP

`CONFIG_MINIHUB_MCP_SERVER` is default n. Enable only with `git submodule update --init components/esp-claw`.

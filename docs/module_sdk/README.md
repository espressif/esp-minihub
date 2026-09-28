# Module SDK (Store plugins)

| Path | Purpose |
|------|---------|
| `include/` | Standalone L3 headers (`minihub_plugin_sdk.h`) — compile without IDF or Host tree |
| `host_export_allowlist.json` | Host ELF symbols L3 `.so` may leave undefined |
| `minihub_plugin_standalone.cmake` | Optional no-IDF CMake fragment |
| `build_plugin_standalone.py` | Discover + cmake + pack (L3 cmake, then L1/L2 zip) |
| `pack_store_module.py` | MHMD wrap + `.mhub` zip (`store_paths.py` alongside) |
| `sync_store_catalog.py` | Rewrite `catalog.json` from deploy `.mhub` files |
| `templates/l1/` | L1 pack (`inject` into mqtt) |
| `templates/l2/` | L2 JS (`main.js`) |
| `templates/l3/` | L3 native (`minihub_module_entry`) |

```bash
python3 tools/new_store_module.py --tier l2 --id my_js --name "My JS"
python3 tools/pack_store_module.py pack my_js
python3 tools/sync_store_catalog.py
```

Do not copy an existing vendor plugin as a template. Agent workflow: `AGENTS.md` and `.agent/skills/minihub-plugin-l*/`.

## IDE MCP (plugin authors)

Stdio server for Cursor / VS Code. It is **not** `CONFIG_MINIHUB_MCP_SERVER` (Hub tools for a running device).

```bash
python3 tools/minihub_plugin_mcp.py
```

This checkout already lists it in `.cursor/mcp.json` and `.vscode/mcp.json`.

| Tool | Purpose |
|------|---------|
| `plugin_guide` | L1 / L2 / L3 / `host.*` / pack rules |
| `scaffold_plugin` | Template files (optional write to `out_dir`) |
| `validate_plugin` | `module.json` id/kind/inject + L2 JS bans |
| `pack_plugin` | Zip L1/L2 into `.mhub` (L3 needs an existing `.so`) |

Plugin-only workspace (no Host clone): still run this script from an `esp-minihub` tree (or copy `tools/plugin_mcp/` + `docs/module_sdk/templates/` + `tools/minihub_plugin_mcp.py`), then `scaffold_plugin` with `out_dir` pointing at the customer's folder.

Tests: `python3 tools/plugin_mcp/test_plugin_mcp.py`.

## L3 out-of-tree compile

Standalone headers for compiling a native plugin **without** checking out
ESP-IDF or the MiniHub firmware tree.

1. This directory (`include/`) plus `host_export_allowlist.json`
2. `minihub_plugin_standalone.cmake` (optional CMake fragment)
3. `build_plugin_standalone.py`, `pack_store_module.py`, `store_paths.py`, and `sync_store_catalog.py` (next to this cmake; in git they symlink to `tools/`). Standalone consumers unzip `module_sdk.zip` and never need an `esp-minihub` clone. After `.mhub` files are in the store directory, run `python3 sync_store_catalog.py` to rewrite `catalog.json` from those zips.
4. A cross compiler that matches Host (ESP32-S31: `riscv32-esp-elf-gcc`)

You do **not** need IDF component headers or MiniHub `components/` on the
include path.

Host firmware with `CONFIG_MINIHUB_DYNAMIC_MODULES` runs
`tools/check_plugin_sdk.py` so these headers cannot drift from IDF/MiniHub
layouts or the export allowlist.

```
include/
  minihub_plugin_sdk.h           # umbrella
  minihub_plugin_sdk/
    esp_idf.h
    lwip.h
    cjson.h
    mbedtls.h
    psa.h
    freertos.h
    minihub.h
```

```c
#include "minihub_plugin_sdk.h"
```

```bash
riscv32-esp-elf-gcc -std=gnu17 -fPIC -shared -nostdlib -nostartfiles \
  -fdata-sections -ffunction-sections -fvisibility=hidden \
  -Wl,-z,max-page-size=4 -Wl,-z,common-page-size=4 \
  -Wl,--gc-sections -Wl,--allow-shlib-undefined \
  -I /path/to/module_sdk/include \
  -o my_native.so module_entry.c my_native_adapter.c
```

Host resolves undefined symbols at `dlopen` from `host_export_allowlist.json`.
Do not link the Host ELF.

In-tree plugins that already build inside IDF include only the MiniHub/IDF
headers they need. Do not mix this SDK header with IDF headers in the same file.

# Documentation source folder

RST sources for the MiniHub Programming Guide, built with
[esp-docs](https://github.com/espressif/esp-docs) (same toolchain as
[esp-idf/docs](https://github.com/espressif/esp-idf/tree/master/docs) and
[esp-matter/docs](https://github.com/espressif/esp-matter/tree/main/docs)).

Scripts under `tools/`: [tools/README.md](../tools/README.md).

Agent policy: root [AGENTS.md](../AGENTS.md).

This guide is the **primary artifact for open-source secondary development**:
architecture, OEM Host, Store plugins, factory/OTA, and REST/MCP/C/JS contracts —
without reading Core C sources.

* English: [docs/en/](en/index.rst)
* Chinese: [docs/zh_CN/](zh_CN/index.rst)

## Audience (where to start)

| Goal | First chapters |
|------|------------------|
| Understand design and advantages | [introduction.rst](en/introduction.rst) → [architecture.rst](en/architecture.rst) |
| Build and flash | [getting_started.rst](en/getting_started.rst) |
| OEM Host product | [application/](en/application/index.rst) → [host_customization.rst](en/host_customization.rst) → [production.rst](en/production.rst) |
| Store plugin (L1/L2) | [plugins/overview.rst](en/plugins/overview.rst) |
| REST / automation only | [type_catalog.rst](en/type_catalog.rst) → [data_model.rst](en/data_model.rst) → [api/](en/api/index.rst) |
| Contribute | [contributing.rst](en/contributing.rst) |

## Building

Output is always **`docs/_build/`** (run `build-docs` with `docs/` as the working
directory). Do **not** use ``--source-dir docs`` from the repository root — that
creates a stray ``/_build/`` at the repo root.

From the repository root (recommended):

```bash
python3 -m venv .venv-docs
source .venv-docs/bin/activate
pip install -r docs/requirements.txt
# C API pages are generated from header Doxygen comments (requires doxygen on PATH)
brew install doxygen   # macOS; or apt install doxygen
./tools/build_docs.sh          # en + zh_CN
./tools/build_docs.sh zh_CN    # Chinese only
```

``./tools/build_docs.sh`` uses ``.venv-docs`` automatically if that directory exists.

Or manually inside ``docs/``:

```bash
cd docs
pip install -r requirements.txt
build-docs -bs html -t esp32s31 -l en
build-docs -bs html -t esp32s31 -l zh_CN
```

HTML output: `docs/_build/en/esp32s31/html/` and `docs/_build/zh_CN/esp32s31/html/`.

HTML builds include a downloadable type-catalog snapshot under
`_static/catalog/` (CSV, device icons, `STANDARD_TYPES*.md`) and
`_static/catalog.zip`, copied from
`components/minihub_device_widget/` at Sphinx configure time. While
`HIDE_L3` is true in `docs/_extensions/omit_elf_plugin_docs.py`, the HTML
build omits L3 chapters and any passage wrapped in `.. only:: not hide_l3`,
drops the raw `_sources/` copies (they would repeat the hidden passages), and
stages figures that leave out the native band. Set `HIDE_L3 = False` to publish
them again; the `.rst` sources stay in the tree either way.

Open locally, for example: `docs/_build/zh_CN/esp32s31/html/index.html`.

HTTP preview (after a successful build):

```bash
python3 tools/preview_docs.py          # http://127.0.0.1:8765/zh_CN/
python3 tools/preview_docs.py --open # also open the browser
python3 tools/preview_docs.py --build-dir ~/website/MiniHub_docs --host 0.0.0.0
```

`/` redirects to ``/zh_CN/`` (``--lang en`` to prefer English). Rebuild after RST or Doxygen header changes.

`build-docs` treats Sphinx warnings as failures (same as CI). Avoid `` ``
literals that contain `<...>` (Sphinx treats them as roles). Prefer `APP` /
`ID` placeholders. CJK titles need underlines at least as wide as the title
(East-Asian width).

Homebrew Doxygen 1.18 on macOS sometimes exits with SIGBUS before it writes
XML or a warning log. That is the Doxygen binary, not an RST error.
``./tools/build_docs.sh`` retries the crash once. If the same language fails
twice, run ``./tools/build_docs.sh en`` or ``./tools/build_docs.sh zh_CN``
again.

## C API documentation (Doxygen → esp-docs)

C API pages (`api/minihub_core`, `api/minihub_adapter`, `api/module_abi`, `api/net_api`) are **generated from
header comments**, same pipeline as ESP-IDF:

1. Write Doxygen in `components/**/include/*.h` (`@brief`, `@param`, `@return`)
2. List new headers in `docs/Doxyfile` `INPUT`
3. RST pages use ``.. include-build-file:: inc/<header>.inc``
4. Run `./tools/build_docs.sh`

Headers currently in `INPUT`: `minihub_core.h`, `minihub_http_worker.h`,
`minihub_adapter.h`, `minihub_module_abi.h`, `minihub_net.h`, `minihub_net_cfg.h`,
`minihub_ble_gatt.h`, and Plugin SDK category headers under
`docs/module_sdk/include/minihub_plugin_sdk/` (`esp_idf.h`, `lwip.h`, `cjson.h`,
`mbedtls.h`, `psa.h`, `freertos.h`). MiniHub types for out-of-tree plugins are
documented from the component headers (not the SDK copy) to avoid duplicates.

`minihub_plugin_sdk.h` lives under `docs/module_sdk/` (`en/api/plugin_host.rst`).
That page is not in the HTML build while `HIDE_L3` is true.
Host builds with `CONFIG_MINIHUB_DYNAMIC_MODULES` run `tools/check_plugin_sdk.py`.

REST / JS / MCP pages remain hand-written RST (no C headers).

## Layout

```
docs/
  README.md / README_CN.md
  requirements.txt
  conf_common.py
  Doxyfile            # Doxygen input list (components headers)
  doxygen-known-warnings.txt
  _static/
  _build/             # esp-docs HTML 输出（git 忽略）
  en/                 # English RST
  zh_CN/              # Chinese RST (same relative paths as en/)
  module_sdk/         # Host export allowlist
  superpowers/        # Internal design notes (not in the Sphinx toctree)
```

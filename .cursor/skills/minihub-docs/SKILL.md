---
name: minihub-docs
description: Write or update MiniHub programming guide (esp-docs RST en/zh_CN, or Markdown fallback). Use when the user edits docs/, RST chapters, API reference, translations, or docs CI.
---

# MiniHub documentation

## Which tree

- Edit **en + zh_CN** with the same relative `.rst` path (`docs/en/`, `docs/zh_CN/`).
- Folder map: `docs/README.md`.

## Build RST

```bash
python3 -m venv .venv-docs   # first time
# ./tools/build_docs.sh activates .venv-docs when present
./tools/build_docs.sh        # en + zh_CN; warnings = failure
```

Needs `doxygen` on PATH for C API pages.

## Style

- RST: no ``literals`` containing `<...>`. CJK titles: underline at least as wide as the title.
- CJK inline markup (measured with docutils for `` x ``, **bold**, *em*, `:doc:`; Sphinx warnings are
  fatal here): an inline markup **end-string needs a real boundary after it** — whitespace, or
  `。 ， 、 ： ） ；`. An opening bracket `（ ( 「` is not a boundary, and neither is a CJK word
  character (`**粗**阻塞`). Insert a space. The reported line is the *paragraph* start, not the offender,
  and the same bad shape often renders clean when a later pair in the block rescues the open — so fix
  what the gate flags and re-run the gate; do not sweep by grep.
- `./tools/build_docs.sh` generates a target sdkconfig from the Host app, so it needs `IDF_PATH` and,
  whenever the feature list pulls in Matter, `ESP_MATTER_PATH` — otherwise it dies before Sphinx runs.
- Align with code:
  - Store `kind` is `module` or `pack`; Store config field `custom_url`
  - L1 / L2 / L3 are **Store packaging tiers**, not Host Core layers
  - Host = Core + bundled adapters; demos under `examples/plugins` + `MINIHUB_PLUGIN_DIRS`; default `store_preinstall: []`
  - Core type-agnostic: one device = one `minihub_point_t`, params in `props.params`; catalog CSV → `/data/config/devices.csv`; widgets toggle/slider/dropdown/text/hidden
  - L2 `host.command.invoke` accepts `{ point_id, cmd, params? }`; use
    `host.command.on(adapter, fn)` and `point.register({ params|props })` for
    Devices-page ownership from JS
- Do not document `application/claw_minihub` or `CONFIG_MINIHUB_NET_PREFER_ETH`. Default IPv4 route: `minihub_net_refresh_default_netif()`.
- Host stays ecosystem-agnostic in prose (vendor examples belong in plugin chapters).

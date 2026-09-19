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
- zh_CN: do not put `**bold**` immediately before a fullwidth `（`.
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

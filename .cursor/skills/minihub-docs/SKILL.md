---
name: minihub-docs
description: Write or update MiniHub programming guide (esp-docs RST en/zh_CN, or Markdown fallback). Use when the user edits docs/, RST chapters, API reference, translations, or docs CI.
---

# MiniHub documentation

## Which tree

- If `docs/en/index.rst` exists: edit **en + zh_CN** with the same relative `.rst` path.
- Else: edit `docs/build.md`, `docs/plugins_and_store.md`, `docs/README.md`.

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
- Align with code: Store `kind` is `module` or `pack`; Store config field `custom_url`; L2 `host.command.invoke` is `{ point_id, cmd }` only.
- Do not document `application/claw_minihub` or `CONFIG_MINIHUB_NET_PREFER_ETH`. Default IPv4 route: `minihub_net_refresh_default_netif()`.
- Host stays ecosystem-agnostic in prose (vendor examples belong in plugin chapters).

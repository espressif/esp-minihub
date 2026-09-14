# Port claw_minihub into esp-minihub

## Goal

Add product `application/claw_minihub` (Claw shell + MiniHub Core + local MCP) following the public Host architecture: Claw lives under `application/`; MIoT remains a Store L3 plugin (not statically linked into the claw firmware).

## Decisions

| Topic | Choice |
|-------|--------|
| Source | Copy from `MiniHub/application/claw_minihub`, then sanitize |
| MIoT | Store `.mhub` only; no `EXTRA_COMPONENT_DIRS` for `minihub_plugins/miot`; no fatfs preseed of `minihub/modules/miot/` |
| Credentials | No `CONFIG_MINIHUB_MIOT_*` preload in Kconfig |
| `cap_account_bind` | **B**: keep non-MIoT (WeChat HTTP/cmd paths); remove built-in MIoT provider |
| `cmd_miot` | Optional lab console via `minihub_module_invoke_rpc("miot", …)` when plugin is loaded |
| `esp-claw` | Git submodule (already in `.gitmodules`) |

## Non-goals

- Changing default Host (`application/minihub`) behavior
- Baking vendor cloud tokens into public config
- Statically shipping MIoT inside claw firmware

## Acceptance

1. `./tools/build_firmware.py -app claw_minihub build` succeeds (with IDF env).
2. `./tools/build_firmware.py -app minihub build` still works without requiring a populated claw tree for Host-only builds (MCP path dep only when building claw).
3. MIoT works only after Store install of the plugin (serial `cmd_miot` / Web Store).

## Follow-up (2026-08-26)

Approach A: removed `cmd_miot`; added plugin-agnostic `module --list/--rpc`. Vendor plugins optional.

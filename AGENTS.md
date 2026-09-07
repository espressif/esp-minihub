# Agent instructions (MiniHub)

For any AI coding assistant. Prefer this file and `docs/` over IDE-only config when they conflict.

## Git commits / PRs

- Do **not** add tool attribution: no `Co-authored-by: Cursor`, no `Made-with: Cursor`, no AI co-author trailers.
- Conventional commits; author stays the human `user.name` / `user.email`.
- Branch names: underscores only (no hyphens), e.g. `feature/modbus_tcp_client`.

## Plugin / adapter / Store work

1. Read **[docs/plugins_and_store.md](docs/plugins_and_store.md)**
2. Build (product = app folder): **[docs/build.md](docs/build.md)**
3. Templates and allowlist: `docs/module_sdk/`, `components/store_modules/README.md`
4. End-user / lab notes belong under the app only when present — **not** as MIoT Host docs.

Host firmware must stay **ecosystem-agnostic**: do not bake vendor cloud tokens, default adapters, or vendor-specific rules into Core.

## Products (applications)

| Product | Notes |
|---------|--------|
| `application/minihub` | Public Host (default) |

Optional on-device MCP tools: `components/minihub_mcp_server`, enabled with
`CONFIG_MINIHUB_MCP_SERVER` in menuconfig (requires `components/esp-claw` submodule).

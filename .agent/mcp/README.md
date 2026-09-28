# Shared MCP configs

Versioned, tool-neutral home for the repo's **IDE-side** MCP server definitions.
Each tool gets one file here named `<tool>.mcp.json`, because the JSON envelope
differs per tool; the tool's own config path is a symlink to it.

| File | Symlinked from | Envelope |
|------|----------------|----------|
| `cursor.mcp.json` | `.cursor/mcp.json` | `mcpServers` |
| `vscode.mcp.json` | `.vscode/mcp.json` | `servers` + `type`, `${workspaceFolder}` |

## Servers

- **`minihub-plugin`** — `python3 tools/minihub_plugin_mcp.py` (stdio). For plugin
  authors: `plugin_guide`, `scaffold_plugin`, `validate_plugin`, `pack_plugin`.

This is **not** the Hub's on-device MCP server (`CONFIG_MINIHUB_MCP_SERVER=y` +
`components/esp-claw`). See `AGENTS.md`.

## Adding or changing a server

Edit every `<tool>.mcp.json` in this directory — the symlinks mean the tools read
these files directly, no copy step. A new tool: add `<tool>.mcp.json` in its
envelope, then symlink it to the path that tool expects.

No credentials here. A server that needs a secret takes it from the environment,
not from `args`.

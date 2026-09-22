# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""JSON-RPC MCP stdio server for Store plugin authors."""

from __future__ import annotations

import json
import sys
import traceback
from typing import Any

from . import SERVER_NAME, __version__
from .knowledge import TOPICS, guide
from .pack_local import pack_plugin_dir
from .scaffold import TIERS, scaffold_plugin
from .validate import validate_module_json, validate_plugin_dir

PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")

INSTRUCTIONS = (
    "MiniHub Store plugin MCP (not the on-device Hub MCP). "
    "Help the user write L1 packs, L2 JS, or L3 native modules. "
    "Call plugin_guide and js_api before inventing APIs. "
    "Ids use underscores, never hyphens. "
    "Do not call esp_wifi_* from JS. "
    "host.command.invoke is {point_id, cmd} only."
)


def _schema(properties: dict, required: list[str] | None = None) -> dict:
    out: dict[str, Any] = {
        "type": "object",
        "properties": properties,
        "additionalProperties": False,
    }
    if required:
        out["required"] = required
    return out


TOOLS = [
    {
        "name": "plugin_guide",
        "description": (
            "MiniHub Store plugin rules for the given topic. "
            "Use before scaffolding or inventing host.* / module.json fields."
        ),
        "inputSchema": _schema(
            {
                "topic": {
                    "type": "string",
                    "enum": list(TOPICS),
                    "description": "Guide section",
                }
            },
            ["topic"],
        ),
    },
    {
        "name": "scaffold_plugin",
        "description": (
            "Generate L1/L2/L3 template files. Returns file contents so the client "
            "can write them without this firmware tree. Optionally write to out_dir "
            "or in-tree examples/plugins/<id>/."
        ),
        "inputSchema": _schema(
            {
                "tier": {"type": "string", "enum": list(TIERS)},
                "id": {
                    "type": "string",
                    "description": "module.json id / directory name (lowercase, underscores)",
                },
                "name": {"type": "string", "description": "Display name (default: id)"},
                "out_dir": {
                    "type": "string",
                    "description": "If set, write files here (any workspace folder)",
                },
                "in_tree": {
                    "type": "boolean",
                    "description": "Write to esp-minihub examples/plugins/<id>/",
                },
                "force": {"type": "boolean", "description": "Overwrite existing out_dir"},
            },
            ["tier", "id"],
        ),
    },
    {
        "name": "validate_plugin",
        "description": "Validate a plugin directory (module.json + L2 JS rules) or a module.json object.",
        "inputSchema": _schema(
            {
                "dir": {"type": "string", "description": "Path to the module directory"},
                "module_json": {
                    "type": "object",
                    "description": "Inline module.json when dir is not available",
                },
            }
        ),
    },
    {
        "name": "pack_plugin",
        "description": (
            "Zip an L1/L2 module directory into <id>.mhub. "
            "Native/hybrid requires an existing .so in that directory (this tool does not compile)."
        ),
        "inputSchema": _schema(
            {
                "dir": {"type": "string", "description": "Module directory containing module.json"},
                "out": {
                    "type": "string",
                    "description": "Output .mhub path (default: <dir>/<id>.mhub)",
                },
            },
            ["dir"],
        ),
    },
]


def _text(payload: Any, *, is_error: bool = False) -> dict[str, Any]:
    if isinstance(payload, str):
        text = payload
    else:
        text = json.dumps(payload, indent=2, ensure_ascii=False)
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


def call_tool(name: str, arguments: dict[str, Any] | None) -> dict[str, Any]:
    args = arguments or {}
    try:
        if name == "plugin_guide":
            return _text(guide(str(args.get("topic") or "overview")))
        if name == "scaffold_plugin":
            result = scaffold_plugin(
                tier=str(args.get("tier") or ""),
                module_id=str(args.get("id") or ""),
                name=str(args.get("name") or ""),
                out_dir=args.get("out_dir") or None,
                in_tree=bool(args.get("in_tree")),
                force=bool(args.get("force")),
            )
            return _text(result)
        if name == "validate_plugin":
            directory = args.get("dir")
            if directory:
                return _text(validate_plugin_dir(directory))
            raw = args.get("module_json")
            if isinstance(raw, dict):
                issues = validate_module_json(raw)
                errors = [i for i in issues if i["level"] == "error"]
                return _text({"ok": not errors, "issues": issues})
            return _text("pass dir or module_json", is_error=True)
        if name == "pack_plugin":
            from pathlib import Path

            out = args.get("out")
            return _text(pack_plugin_dir(Path(str(args["dir"])), Path(out) if out else None))
        return _text(f"unknown tool {name!r}", is_error=True)
    except (OSError, ValueError, FileNotFoundError, FileExistsError, TypeError) as exc:
        return _text(str(exc), is_error=True)
    except Exception as exc:  # noqa: BLE001 — surface unexpected errors to the IDE
        return _text(f"{exc}\n{traceback.format_exc()}", is_error=True)


def _initialize(params: dict[str, Any] | None) -> dict[str, Any]:
    requested = (params or {}).get("protocolVersion") or PROTOCOL_VERSIONS[0]
    version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[-1]
    return {
        "protocolVersion": version,
        "capabilities": {"tools": {"listChanged": False}},
        "serverInfo": {"name": SERVER_NAME, "version": __version__},
        "instructions": INSTRUCTIONS,
    }


def handle(message: dict[str, Any]) -> dict[str, Any] | None:
    method = message.get("method")
    msg_id = message.get("id")
    params = message.get("params") if isinstance(message.get("params"), dict) else {}

    if method and str(method).startswith("notifications/"):
        return None
    if msg_id is None:
        return None

    try:
        if method == "initialize":
            result = _initialize(params)
        elif method == "ping":
            result = {}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            result = call_tool(str(params.get("name") or ""), params.get("arguments"))
        elif method in ("resources/list", "prompts/list"):
            result = {"resources": []} if method.startswith("resources") else {"prompts": []}
        else:
            return {
                "jsonrpc": "2.0",
                "id": msg_id,
                "error": {"code": -32601, "message": f"Method not found: {method}"},
            }
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}
    except Exception as exc:  # noqa: BLE001
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "error": {"code": -32603, "message": str(exc)},
        }


def serve_stdio() -> None:
    stdin = sys.stdin
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(message, dict):
            continue
        reply = handle(message)
        if reply is None:
            continue
        sys.stdout.write(json.dumps(reply, ensure_ascii=False) + "\n")
        sys.stdout.flush()

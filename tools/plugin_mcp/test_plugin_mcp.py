# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from plugin_mcp.knowledge import guide
from plugin_mcp.pack_local import pack_plugin_dir
from plugin_mcp.scaffold import scaffold_plugin
from plugin_mcp.server import call_tool, handle
from plugin_mcp.validate import validate_module_json, validate_plugin_dir


class PluginMcpTests(unittest.TestCase):
    def test_guide_unknown(self) -> None:
        text = guide("nope")
        self.assertIn("unknown topic", text)

    def test_id_hyphen_rejected(self) -> None:
        issues = validate_module_json({"id": "foo-bar", "kind": "pack", "runtime": "pack"})
        self.assertTrue(any("hyphen" in i["message"] or "invalid id" in i["message"] for i in issues))

    def test_inject_unknown_adapter(self) -> None:
        issues = validate_module_json(
            {
                "id": "my_pack",
                "kind": "pack",
                "runtime": "pack",
                "inject": {"adapter": "new_radio"},
            }
        )
        self.assertTrue(any("new_radio" in i["message"] for i in issues))

    def test_rpc_initialize_and_tools(self) -> None:
        init = handle(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "t"}},
            }
        )
        assert init is not None
        self.assertEqual(init["result"]["serverInfo"]["name"], "minihub-plugin")
        listed = handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        assert listed is not None
        names = {t["name"] for t in listed["result"]["tools"]}
        self.assertEqual(names, {"plugin_guide", "scaffold_plugin", "validate_plugin", "pack_plugin"})

    def test_scaffold_l2_and_pack(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "hello_js"
            result = scaffold_plugin(tier="l2", module_id="hello_js", name="Hello", out_dir=str(dest))
            self.assertTrue((dest / "main.js").is_file())
            self.assertIn("hello_js", result["files"]["main.js"])
            report = validate_plugin_dir(dest)
            self.assertTrue(report["ok"], report)
            packed = pack_plugin_dir(dest)
            self.assertTrue(packed["ok"], packed)
            with zipfile.ZipFile(packed["pack"]) as zf:
                self.assertIn("module.json", zf.namelist())
                self.assertIn("main.js", zf.namelist())

    def test_js_forbidden_token(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "bad_js"
            scaffold_plugin(tier="l2", module_id="bad_js", out_dir=str(dest), force=True)
            (dest / "main.js").write_text("esp_wifi_start();\n", encoding="utf-8")
            report = validate_plugin_dir(dest)
            self.assertFalse(report["ok"])

    def test_tool_guide(self) -> None:
        out = call_tool("plugin_guide", {"topic": "js_api"})
        self.assertFalse(out["isError"])
        self.assertIn("host.command.invoke", out["content"][0]["text"])

    def test_notification_no_reply(self) -> None:
        self.assertIsNone(
            handle({"jsonrpc": "2.0", "method": "notifications/initialized"})
        )


if __name__ == "__main__":
    unittest.main()

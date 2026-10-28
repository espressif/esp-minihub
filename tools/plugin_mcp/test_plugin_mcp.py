# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from pack_store_module import MHMD_MAGIC
from plugin_mcp.knowledge import guide
from plugin_mcp.pack_local import pack_plugin_dir
from plugin_mcp.scaffold import scaffold_plugin
from plugin_mcp.server import call_tool, handle
from plugin_mcp.validate import validate_js_source, validate_module_json, validate_plugin_dir


class PluginMcpTests(unittest.TestCase):
    def test_guide_unknown(self) -> None:
        text = guide("nope")
        self.assertIn("unknown topic", text)

    def test_id_hyphen_rejected(self) -> None:
        issues = validate_module_json({"id": "foo-bar", "type": "pack", "runtime": "pack"})
        self.assertTrue(any("hyphen" in i["message"] or "invalid id" in i["message"] for i in issues))

    def test_inject_unknown_adapter(self) -> None:
        issues = validate_module_json(
            {
                "id": "my_pack",
                "type": "pack",
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

    def test_adapter_required_like_loader(self) -> None:
        """The device rejects a manifest with no adapter — so must we."""
        issues = validate_module_json(
            {"id": "my_js", "type": "module", "runtime": "js"}
        )
        self.assertTrue(
            any(i["level"] == "error" and "adapter" in i["message"] for i in issues),
            issues,
        )

    def test_runtime_not_inferred_from_main_js(self) -> None:
        """minihub_js only starts QuickJS for runtime "js"/"hybrid"."""
        issues = validate_module_json({"id": "my_js", "type": "module", "adapter": "my_js"})
        self.assertTrue(
            any(i["level"] == "error" and "runtime" in i["message"] for i in issues),
            issues,
        )

    def test_command_invoke_params_is_legal(self) -> None:
        """minihub_js.c reads params, so flagging it is a false positive."""
        self.assertEqual(
            validate_js_source(
                "function setup(host) {\n"
                '  host.command.invoke({ point_id: "a.b", cmd: "set", params: { v: 1 } });\n'
                "}\n"
            ),
            [],
        )

    def test_js_without_setup_and_top_level_host_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "no_setup"
            scaffold_plugin(tier="l2", module_id="no_setup", out_dir=str(dest), force=True)
            (dest / "main.js").write_text(
                'host.log.info("loaded");\n', encoding="utf-8"
            )
            msgs = " ".join(i["message"] for i in validate_plugin_dir(dest)["issues"])
            self.assertIn("setup", msgs)
            self.assertIn("file level", msgs)

    def test_l2_template_satisfies_the_loader(self) -> None:
        """A scaffolded L2 must actually load: adapter present, setup(host) defined,
        and no host.* use before setup."""
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "scaffold_js"
            result = scaffold_plugin(tier="l2", module_id="scaffold_js", out_dir=str(dest))
            manifest = json.loads((dest / "module.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest.get("adapter"), "scaffold_js")
            self.assertEqual(manifest.get("runtime"), "js")
            self.assertIn("function setup(host)", result["files"]["main.js"])
            report = validate_plugin_dir(dest)
            self.assertTrue(report["ok"], report)

    def test_pack_carries_meta_json(self) -> None:
        """meta.json is the canonical home of ui + capabilities."""
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "pack_js"
            scaffold_plugin(tier="l2", module_id="pack_js", out_dir=str(dest))
            src_manifest = json.loads((dest / "module.json").read_text(encoding="utf-8"))
            src_manifest.update(abi_version=1, core_api_min=1, core_api_max=1)
            (dest / "module.json").write_text(
                json.dumps(src_manifest), encoding="utf-8"
            )
            packed = pack_plugin_dir(dest)
            self.assertTrue(packed["ok"], packed)
            with zipfile.ZipFile(packed["pack"]) as zf:
                self.assertIn("meta.json", zf.namelist())
                # ui/capabilities must not stay in the packaged module.json
                manifest = json.loads(zf.read("module.json").decode("utf-8"))
                self.assertNotIn("capabilities", manifest)
                self.assertNotIn("ui", manifest)
                self.assertNotIn("abi_version", manifest)
                self.assertNotIn("core_api_min", manifest)
                self.assertNotIn("core_api_max", manifest)

    def test_pack_wraps_native_so_with_mhmd(self) -> None:
        """The device validates the MHMD header on the packed .so — a bare linker
        output installs with ESP_ERR_INVALID_CRC."""
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "my_native"
            dest.mkdir()
            (dest / "module.json").write_text(
                json.dumps(
                    {
                        "id": "my_native",
                        "name": "Native",
                        "version": "0.1.0",
                        "type": "module",
                        "runtime": "native",
                        "adapter": "my_native",
                        "binary": "my_native.so",
                    }
                ),
                encoding="utf-8",
            )
            (dest / "my_native.so").write_bytes(b"\x7fELF" + b"\x00" * 64)
            packed = pack_plugin_dir(dest)
            self.assertTrue(packed["ok"], packed)
            with zipfile.ZipFile(packed["pack"]) as zf:
                payload = zf.read("my_native.so")
                manifest = json.loads(zf.read("module.json").decode("utf-8"))
            self.assertEqual(struct.unpack("<I", payload[:4])[0], MHMD_MAGIC)
            self.assertEqual(manifest.get("binary"), "my_native.so")
            self.assertEqual(manifest.get("binary_size"), len(payload))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Regression tests for the LCD UI visual editor (tools/lcd_ui_editor/).

The editor rewrites Brookesia JSON assets that a human (or another agent) owns.
Two properties must not regress:

1. **Byte-exact round trip.** Re-emitting an unchanged asset must reproduce the
   file byte for byte. The assets mix three layouts (expanded, ``{"a": 1}`` and
   ``{ "a": 1 }``); without per-container style tracking a no-op save turns
   into a whole-file reformat diff.
2. **Transactional save.** When the contract suite fails, every file is put back
   exactly as it was. A broken UI must not survive on disk.

The write-side tests run against a TEMP copy of the UI directory, so the suite
never touches the working tree.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "lcd_ui_editor"))

import server  # noqa: E402  (path set above)

UI_DIR = ROOT / "components" / "minihub_lcd" / "ui"

# `unittest -v` prints: "FAIL: test_x (pkg.mod.Class.test_x)"
UI_FAILURE_OUTPUT = (
    "FAIL: test_every_declared_view_exists "
    "(tools.test_lcd_ui_contract.ContractCoverageTest.test_every_declared_view_exists)\n")
CPP_FAILURE_OUTPUT = (
    "FAIL: test_no_bare_action_names_left_in_cpp "
    "(tools.test_lcd_ui_contract.NoBareLiteralsTest.test_no_bare_action_names_left_in_cpp)\n")


class RoundTripTest(unittest.TestCase):
    """Reading an asset and writing it back must not change a byte."""

    def test_every_asset_round_trips_byte_exactly(self):
        assets = sorted(UI_DIR.rglob("*.json"))
        self.assertTrue(assets, "no UI assets found")
        for path in assets:
            with self.subTest(asset=str(path.relative_to(UI_DIR))):
                text = path.read_text(encoding="utf-8")
                out = server.render(json.loads(text), server.inline_paths(text))
                if text.endswith("\n"):
                    out += "\n"
                self.assertEqual(text, out, f"{path.name} does not round-trip")

    def test_inline_styles_are_detected(self):
        tight = '{\n  "a": {"k": 1},\n  "b": [{"x": 2}]\n}\n'
        padded = '{\n  "a": { "k": 1 }\n}\n'
        self.assertEqual(server.inline_paths(tight)[("a",)], "tight")
        self.assertEqual(server.inline_paths(padded)[("a",)], "padded")
        # An expanded container is not inline at all.
        self.assertNotIn(("a",), server.inline_paths('{\n  "a": {\n    "k": 1\n  }\n}\n'))


class WriteTest(unittest.TestCase):
    """Write-side behaviour, on a temp copy - never on the working tree."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.ui = Path(self.tmp.name) / "ui"
        shutil.copytree(UI_DIR, self.ui)
        self._ui_dir = server.UI_DIR
        server.UI_DIR = self.ui

    def tearDown(self):
        server.UI_DIR = self._ui_dir
        self.tmp.cleanup()

    def test_unchanged_content_is_not_rewritten(self):
        target = self.ui / "screens" / "overview.json"
        before = target.stat().st_mtime_ns
        data = json.loads(target.read_text(encoding="utf-8"))
        self.assertFalse(server.write_preserving(target, data),
                         "an unchanged file must not be rewritten")
        self.assertEqual(before, target.stat().st_mtime_ns)

    def test_a_changed_file_keeps_the_original_layout(self):
        target = self.ui / "templates" / "device_card.json"
        text = target.read_text(encoding="utf-8")
        data = json.loads(text)
        data["node"]["layout"]["gap"] = "6dp"
        self.assertTrue(server.write_preserving(target, data))
        after = target.read_text(encoding="utf-8")
        # the one-line layout object stays one-line ...
        self.assertIn('"layout": {"type": "flex", "flexFlow": "column", "gap": "6dp"}',
                      after)
        # ... and the rest of the file keeps its shape
        self.assertEqual(len(text.splitlines()), len(after.splitlines()))

    def test_a_failed_contract_suite_restores_every_file(self):
        root = json.loads((self.ui / "root.json").read_text(encoding="utf-8"))
        assets = {rel: json.loads((self.ui / rel).read_text(encoding="utf-8"))
                  for rel in root["assets"]}
        before = {p: p.read_bytes() for p in self.ui.rglob("*.json")}

        # Break it the way a careless edit would: drop a view the contract
        # declares, so the suite must reject the save.
        overview = assets["screens/overview.json"]
        overview["children"] = [c for c in overview["children"]
                                if c.get("id") != "sidebar"]

        real_suite = server.run_suite
        server.run_suite = lambda: (False, UI_FAILURE_OUTPUT)
        try:
            ok, message, _ = server.save_ui(root, assets)
        finally:
            server.run_suite = real_suite

        self.assertFalse(ok)
        self.assertIn("回滚", message)
        after = {p: p.read_bytes() for p in self.ui.rglob("*.json")}
        self.assertEqual(before, after, "a rejected save must restore every file")

    def test_a_firmware_only_failure_does_not_block_a_ui_save(self):
        """Someone else's in-flight C++ edit must not lock the editor out.

        The suite also checks the firmware C++; editing a JSON asset cannot
        break those, and blocking on them would mean one agent's C++ work
        stops everybody from editing the screen UI.
        """
        root = json.loads((self.ui / "root.json").read_text(encoding="utf-8"))
        assets = {rel: json.loads((self.ui / rel).read_text(encoding="utf-8"))
                  for rel in root["assets"]}

        real_suite = server.run_suite
        server.run_suite = lambda: (False, CPP_FAILURE_OUTPUT)
        try:
            ok, message, _ = server.save_ui(root, assets)
        finally:
            server.run_suite = real_suite

        self.assertTrue(ok, "a C++-only failure must not block the save")
        self.assertIn("与本次改动无关", message)

    def test_paths_outside_the_ui_dir_are_refused(self):
        for bad in ["../ui_contract.json", "/etc/passwd", "screens/../../x.json",
                    "screens/overview.txt", "screens\\overview.json"]:
            with self.subTest(path=bad):
                self.assertIsNone(server.safe_target(bad))

    def test_asset_paths_inside_the_ui_dir_are_accepted(self):
        for good in ["screens/overview.json", "templates/device_card.json",
                     "flows/main.json"]:
            with self.subTest(path=good):
                self.assertIsNotNone(server.safe_target(good))


class PageTest(unittest.TestCase):
    """The page must actually work - not just load without an error."""

    def test_every_dom_id_used_by_the_scripts_exists_in_the_page(self):
        """A missing id used to kill the whole render silently.

        `loadAll()` touched a `#dirty` element that the HTML never had: the
        exception fired BEFORE the first render, so the editor showed an empty
        tree and no error message anywhere. Static cross-check the ids.
        """
        import re
        page = (ROOT / "tools" / "lcd_ui_editor" / "index.html").read_text(
            encoding="utf-8")
        in_page = set(re.findall(r'id="([^"]+)"', page))
        used = set()
        for name in ("app.js", "preview.js"):
            script = (ROOT / "tools" / "lcd_ui_editor" / name).read_text(
                encoding="utf-8")
            used |= set(re.findall(r"getElementById\(['\"]([^'\"]+)['\"]\)", script))
        missing = sorted(used - in_page)
        self.assertEqual([], missing,
                         f"the scripts use ids that index.html does not define: {missing}")

    def test_a_query_string_still_serves_the_page(self):
        """`/?v=1` must serve index.html, not a 404 JSON blob."""
        import http.client
        import threading
        from http.server import ThreadingHTTPServer

        httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", httpd.server_address[1],
                                              timeout=10)
            for path, expect in [("/", 200), ("/?v=1", 200), ("/app.js", 200),
                                 ("/api/ui", 200), ("/nope", 404)]:
                with self.subTest(path=path):
                    conn.request("GET", path)
                    res = conn.getresponse()
                    body = res.read().decode("utf-8")
                    self.assertEqual(expect, res.status)
                    if path in ("/", "/?v=1"):
                        self.assertIn("<div id=\"tree\">", body)
                    if path == "/api/ui":
                        self.assertIn('"assets"', body)
                        self.assertIn('"screen_profile"', body)
        finally:
            httpd.shutdown()
            httpd.server_close()


class AttributionTest(unittest.TestCase):
    """A failure the UI assets cannot cause must not block editing the UI."""

    def test_failed_classes_are_read_from_the_suite_output(self):
        out = ("FAIL: test_no_bare_action_names_left_in_cpp "
               "(tools.test_lcd_ui_contract.NoBareLiteralsTest."
               "test_no_bare_action_names_left_in_cpp)\n")
        self.assertEqual({"NoBareLiteralsTest"}, server.failed_classes(out))

    def test_a_firmware_cpp_failure_does_not_block_a_ui_save(self):
        out = ("FAIL: test_x (tools.test_lcd_ui_contract.NoBareLiteralsTest.test_x)\n")
        self.assertEqual(set(), server.ui_affecting_failures(out))

    def test_a_ui_contract_failure_does_block_a_ui_save(self):
        out = ("FAIL: test_y (tools.test_lcd_ui_contract.ContractCoverageTest.test_y)\n")
        self.assertEqual({"ContractCoverageTest"}, server.ui_affecting_failures(out))


class ScreenProfileTest(unittest.TestCase):
    """The preview's screen geometry must come from the firmware, not a copy."""

    def test_base_geometry_is_parsed_from_the_firmware_source(self):
        p = server.screen_profile()
        self.assertEqual(800, p["base_width"])
        self.assertEqual(480, p["base_height"])
        self.assertEqual(112, p["sidebar_width"])
        self.assertEqual(688, p["main_width"])
        self.assertEqual(1.0, p["density"])
        self.assertEqual(1.0, p["font_scale"])
        self.assertEqual("components/minihub_lcd/minihub_lcd_app.cpp", p["source"])

    def test_a_missing_cpp_source_falls_back_to_defaults(self):
        real = server.CPP_APP
        server.CPP_APP = ROOT / "no-such-cpp-file.cpp"
        try:
            p = server.screen_profile()
        finally:
            server.CPP_APP = real
        self.assertIsNone(p["source"])
        self.assertEqual(800, p["base_width"])

    def test_load_ui_carries_the_profile(self):
        self.assertEqual(800, server.load_ui()["screen_profile"]["base_width"])


# Runs preview.js in a bare VM: no DOM is needed to check the unit conversion,
# which is exactly the part that used to be a hardcoded 1:1 dp -> px mapping.
PROBE_JS = r"""
const fs = require('fs'), vm = require('vm');
const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(process.argv[1], 'utf8'), ctx);
vm.runInContext('pvSetProfile(' + process.argv[2] + ', ' + process.argv[3] + ')', ctx);
const get = (expr) => vm.runInContext(expr, ctx);
process.stdout.write(JSON.stringify({
  W: get('PV.W'), H: get('PV.H'), density: get('PV.density'),
  zoom: get('pvFitZoom()'),
  dp: get("pvNum('12dp', 0)"), sp: get("pvNum('16sp', 0)"),
  pct: get("pvNum('50%', 0)"), px: get("pvNum('32px', 0)"),
  grow: get('pvGrow(1)'),
}));
"""


class DensityTest(unittest.TestCase):
    """Switching resolution must scale dp/sp the way Brookesia parses them."""

    def _probe(self, width, font_scale=1.0):
        import shutil
        import subprocess
        if not shutil.which("node"):
            self.skipTest("node is not installed")
        profile = json.dumps({
            "base_width": 800, "base_height": 480,
            "sidebar_width": 112, "main_width": 688,
            "density": 1.0, "font_scale": font_scale,
        })
        src = ROOT / "tools" / "lcd_ui_editor" / "preview.js"
        proc = subprocess.run(
            ["node", "-e", PROBE_JS, str(src), profile, str(width)],
            capture_output=True, text=True)
        self.assertEqual(0, proc.returncode, proc.stderr)
        return json.loads(proc.stdout)

    def test_base_resolution_is_a_one_to_one_render(self):
        r = self._probe(800)
        self.assertEqual((800, 480, 1.0), (r["W"], r["H"], r["density"]))
        self.assertAlmostEqual(12, r["dp"])
        self.assertAlmostEqual(16, r["sp"])
        self.assertAlmostEqual(50, r["pct"])
        self.assertAlmostEqual(32, r["px"])
        self.assertAlmostEqual(1, r["grow"])
        self.assertAlmostEqual(0.62, r["zoom"])

    def test_a_doubled_resolution_scales_dp_and_sp_only(self):
        r = self._probe(1600)
        self.assertEqual((1600, 960, 2.0), (r["W"], r["H"], r["density"]))
        self.assertAlmostEqual(24, r["dp"])
        self.assertAlmostEqual(32, r["sp"])
        self.assertAlmostEqual(50, r["pct"], "% is resolved against the parent box")
        self.assertAlmostEqual(32, r["px"], "explicit px is never density-scaled")
        self.assertAlmostEqual(1, r["grow"], "flexGrow is a ratio, not a length")
        self.assertAlmostEqual(0.31, r["zoom"], "the preview keeps the same visual size")

    def test_font_scale_only_affects_sp(self):
        r = self._probe(1600, font_scale=1.5)
        self.assertAlmostEqual(24, r["dp"])
        self.assertAlmostEqual(48, r["sp"])


if __name__ == "__main__":
    unittest.main()

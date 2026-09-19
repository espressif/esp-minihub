#!/usr/bin/env python3
"""Unit tests for build_plugin_standalone path helpers (no cross-compiler)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_plugin_standalone as bps  # noqa: E402


class TestResolvePluginDir(unittest.TestCase):
    def test_examples_plugins_id(self):
        d = bps.resolve_plugin_dir("color_bulb", root=ROOT)
        self.assertEqual(d, ROOT / "examples" / "plugins" / "color_bulb")
        self.assertTrue((d / "module.json").is_file())

    def test_unknown_id_raises(self):
        with self.assertRaises(FileNotFoundError):
            bps.resolve_plugin_dir("no_such_plugin_zz", root=ROOT)

    def test_rejects_non_native_runtime(self):
        with self.assertRaises(ValueError):
            bps.resolve_plugin_dir("color_bulb_js", root=ROOT)


if __name__ == "__main__":
    unittest.main()

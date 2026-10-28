#!/usr/bin/env python3
"""Unit tests for build_plugin_standalone path helpers (no cross-compiler)."""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_plugin_standalone as bps  # noqa: E402


class TestResolvePluginDir(unittest.TestCase):
    def _demo_tree(self, module_id: str, module_json: dict) -> Path:
        """A throwaway repo root: no L3 demo ships in-tree, so the native
        plugin this helper must find is built here."""
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        plugin = root / "examples" / "plugins" / module_id
        plugin.mkdir(parents=True)
        (plugin / "module.json").write_text(json.dumps(module_json), encoding="utf-8")
        (plugin / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.16)\n", encoding="utf-8")
        return root

    def test_examples_plugins_id(self):
        root = self._demo_tree("my_native", {"id": "my_native", "runtime": "native"})
        d = bps.resolve_plugin_dir("my_native", root=root)
        self.assertEqual(d, root / "examples" / "plugins" / "my_native")
        self.assertTrue((d / "module.json").is_file())

    def test_unknown_id_raises(self):
        root = self._demo_tree("my_native", {"id": "my_native", "runtime": "native"})
        with self.assertRaises(FileNotFoundError):
            bps.resolve_plugin_dir("no_such_plugin_zz", root=root)

    def test_rejects_non_native_runtime(self):
        root = self._demo_tree("my_js", {"id": "my_js", "runtime": "js"})
        with self.assertRaises(ValueError):
            bps.resolve_plugin_dir("my_js", root=root)

    def test_native_without_cmakelists_raises(self):
        root = self._demo_tree("my_native", {"id": "my_native", "runtime": "native"})
        (root / "examples" / "plugins" / "my_native" / "CMakeLists.txt").unlink()
        with self.assertRaises(FileNotFoundError):
            bps.resolve_plugin_dir("my_native", root=root)


if __name__ == "__main__":
    unittest.main()

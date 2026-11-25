#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Smoke test for tools/pack_store_resources.py (no pytest required)."""
from __future__ import annotations

import tempfile
import zipfile
from pathlib import Path

import pack_store_resources


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="pack-test-") as tmp:
        out = Path(tmp) / "out"
        written = pack_store_resources.pack(
            out_dir=out,
            web_src=root / "components" / "minihub_web" / "web",
            ui_src=root / "components" / "minihub_lcd" / "ui",
            device_widget=root / "components" / "minihub_device_widget",
            sdkconfig=root / "application" / "minihub" / "sdkconfig",
            quiet=True,
        )
        assert written, "no zips written"
        plain = next(p for p in written if p.name.endswith("-plain.zip"))
        gzip_z = next(p for p in written if p.name.endswith("-gzip.zip"))
        with zipfile.ZipFile(plain) as zf:
            names = zf.namelist()
            assert "index.html" in names
            assert "js/boot.js" in names
            assert not any(n.endswith(".gz") for n in names)
            assert not any(".stamp" in n for n in names)
        with zipfile.ZipFile(gzip_z) as zf:
            names = zf.namelist()
            assert "index.html.gz" in names
            assert "js/boot.js.gz" in names
            assert "login.html" in names
            assert "manifest.json" in names
            assert not any(".stamp" in n for n in names)
        print("test_pack_store_resources: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

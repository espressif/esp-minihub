#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Pack Web / LCD UI / Assets into portal-ready ZIP artifacts.

Web deploy layout uses parallel plain/ and gzip/ trees (see minify_web_assets).
This tool emits separate upload ZIPs with content at the archive root:

  minihub-web-<ver>-plain.zip
  minihub-web-<ver>-gzip.zip
  minihub-ui-<ver>.zip
  minihub-assets.zip

Build markers (``.stamp``, ``.DS_Store``, other dotfiles) are never included.
"""
from __future__ import annotations

import argparse
import re
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gen_deploy_index  # noqa: E402
import minify_lcd_ui_assets  # noqa: E402
import minify_web_assets  # noqa: E402

_CFG_WEB = re.compile(r'^CONFIG_MINIHUB_WEB_VERSION="([^"]*)"', re.M)
_CFG_UI = re.compile(r'^CONFIG_MINIHUB_LCD_UI_VERSION="([^"]*)"', re.M)
_CFG_FW = re.compile(r'^CONFIG_MINIHUB_VERSION="([^"]*)"', re.M)


def _skip_path(rel: Path) -> bool:
    for part in rel.parts:
        if part.startswith(".") or part.endswith(".stamp") or part == ".DS_Store":
            return True
    return False


def zip_tree(src: Path, zip_path: Path) -> int:
    """Zip files under src at archive root; skip stamps and dotfiles."""
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(src.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(src)
            if _skip_path(rel):
                continue
            zf.write(path, rel.as_posix())
            n += 1
    return n


def read_versions(sdkconfig: Path | None) -> tuple[str, str]:
    web = "0.1.0"
    ui = "0.1.0"
    if sdkconfig and sdkconfig.is_file():
        text = sdkconfig.read_text(encoding="utf-8", errors="replace")
        m = _CFG_WEB.search(text)
        if m and m.group(1).strip():
            web = m.group(1).strip()
        m = _CFG_UI.search(text)
        if m and m.group(1).strip():
            ui = m.group(1).strip()
        else:
            m = _CFG_FW.search(text)
            if m and m.group(1).strip():
                ui = m.group(1).strip()
    return web, ui


def folder_version(ver: str) -> str:
    v = ver.strip()
    if len(v) > 1 and v[0] in "vV" and v[1].isdigit():
        return v
    return "v" + v


def pack(
    *,
    out_dir: Path,
    web_src: Path,
    ui_src: Path | None,
    device_widget: Path,
    sdkconfig: Path | None,
    quiet: bool,
) -> list[Path]:
    web_ver, ui_ver = read_versions(sdkconfig)
    web_canon = minify_web_assets.canonical_version(web_ver)
    ui_canon = minify_web_assets.canonical_version(ui_ver)
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    with tempfile.TemporaryDirectory(prefix="minihub-pack-") as tmp:
        deploy = Path(tmp) / "deploy"
        deploy.mkdir()

        web_folder = folder_version(web_ver)
        argv = [
            "--src",
            str(web_src),
            "--deploy-root",
            str(deploy),
            "--web-version",
            web_folder,
            "--encoding",
            "both",
            "-q",
        ]
        notes = web_src / "release_notes.txt"
        if notes.is_file():
            argv.extend(["--notes", str(notes)])
        minify_web_assets.main(argv)

        web_root = deploy / "Webs" / web_folder
        plain = web_root / minify_web_assets.PLAIN_DIR
        gzip_dir = web_root / minify_web_assets.GZIP_DIR
        if not plain.is_dir() or not gzip_dir.is_dir():
            raise SystemExit(f"expected {plain} and {gzip_dir}")

        z_plain = out_dir / f"minihub-web-{web_canon}-plain.zip"
        z_gzip = out_dir / f"minihub-web-{web_canon}-gzip.zip"
        n1 = zip_tree(plain, z_plain)
        n2 = zip_tree(gzip_dir, z_gzip)
        written.extend([z_plain, z_gzip])
        if not quiet:
            print(f"web plain zip: {z_plain.name} ({n1} files)")
            print(f"web gzip zip:  {z_gzip.name} ({n2} files)")

        if ui_src and (ui_src / "root.json").is_file():
            ui_folder = folder_version(ui_ver)
            ui_argv = [
                "--src",
                str(ui_src),
                "--deploy-root",
                str(deploy),
                "--ui-version",
                ui_folder,
                "-q",
            ]
            ui_notes = ui_src / "release_notes.txt"
            if ui_notes.is_file():
                ui_argv.extend(["--notes", str(ui_notes)])
            minify_lcd_ui_assets.main(ui_argv)
            ui_root = deploy / "LCD_UI" / ui_folder
            z_ui = out_dir / f"minihub-ui-{ui_canon}.zip"
            n3 = zip_tree(ui_root, z_ui)
            written.append(z_ui)
            if not quiet:
                print(f"ui zip:        {z_ui.name} ({n3} files)")
        elif not quiet:
            print("ui zip:        skipped (no root.json)")

        rc = gen_deploy_index.main(
            [
                "--deploy",
                str(deploy),
                "--device-widget",
                str(device_widget),
                "--sync-source",
                "--only",
                "assets",
            ]
        )
        if rc != 0:
            raise SystemExit(f"gen_deploy_index failed: {rc}")
        assets = deploy / "Assets"
        z_assets = out_dir / "minihub-assets.zip"
        n4 = zip_tree(assets, z_assets)
        written.append(z_assets)
        if not quiet:
            print(f"assets zip:    {z_assets.name} ({n4} files)")

    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=ROOT / "ci_deploy",
        help="directory for ZIP artifacts (default: ci_deploy/)",
    )
    ap.add_argument(
        "--sdkconfig",
        type=Path,
        default=ROOT / "application" / "minihub" / "sdkconfig",
    )
    ap.add_argument(
        "--web-src",
        type=Path,
        default=ROOT / "components" / "minihub_web" / "web",
    )
    ap.add_argument(
        "--ui-src",
        type=Path,
        default=ROOT / "components" / "minihub_lcd" / "ui",
    )
    ap.add_argument(
        "--device-widget",
        type=Path,
        default=ROOT / "components" / "minihub_device_widget",
    )
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)
    if not args.web_src.is_dir():
        raise SystemExit(f"web src missing: {args.web_src}")
    pack(
        out_dir=args.out_dir.resolve(),
        web_src=args.web_src.resolve(),
        ui_src=args.ui_src.resolve() if args.ui_src else None,
        device_widget=args.device_widget.resolve(),
        sdkconfig=args.sdkconfig.resolve() if args.sdkconfig else None,
        quiet=args.quiet,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

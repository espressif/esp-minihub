#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Write deploy-root indexes for plugins / OTA / asset / web.

Generates under a deploy root (default MINIHUB_PLUGINS_DEPLOY_DIR or
~/workspace/minihub_store):

  Web and asset manifests are served dynamically by store_server.py as
  GET /api/web and GET /api/assets (built from Webs/<version>/ and Assets/),
  so no static index.json is written here.
  Assets/device_widgets.json -- per device_id: icon + per-parameter icon/style/meta
  Assets/parameters.json     -- global parameter registry (icon/style/meta)
  Assets/styles/<param>.css  -- one CSS file per parameter (from parameters.csv css column)

Plugin and firmware indexes are NOT generated here: the deploy server aggregates
``Stores/<chip-or-Common>/<id>/catalog.json`` and walks ``Firmwares/<chip>/<version>/``.

Device type data (model/*.csv + images/*) is read from ``--device-widget``.
When the deploy root already has ``Assets/model/*.csv`` it is used instead, so a
single dropped icon can be re-indexed with ``--only assets`` without touching
firmware / plugins / web.

  python3 tools/gen_deploy_index.py --deploy ~/workspace/minihub_website_internal
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import resolve_deploy_dir  # noqa: E402

SCHEMA = "minihub.deploy.index/1"
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEVICE_WIDGET = ROOT / "components" / "minihub_device_widget"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --------------------------------------------------------------------------- #
# asset / web indexes (existing behavior)
# --------------------------------------------------------------------------- #
def _item(*, item_id: str, name: str, version: str, url: str, path: Path, kind_type: str) -> dict:
    return {
        "id": item_id,
        "name": name,
        "version": version,
        "url": url,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
        "targets": ["*"],
        "type": kind_type,
    }


def _asset_item(rel: str, path: Path) -> dict:
    """One asset entry, carrying only what the firmware actually reads.

    The device fetches this index hourly, so the payload is worth trimming:

      url     download + local path derivation (dest_for)
      size    file_matches() staleness check + download verification
      sha256  integrity
      id      dead weight (the firmware only frees it), but the index parser
              rejects an item without it (update_index.c:116) -- kept so a
              fleet still on older firmware keeps parsing the manifest.

    Dropped on purpose: name (freed, never read), targets (absent means
    "matches every chip", update_index.c:222), type (only a fallback for
    prefix-less urls in dest_for; asset urls always carry model/ images/
    styles/), version (assets are all 1.0; the dedup compare is NULL-safe).
    """
    return {
        "id": rel.replace("/", "_"),
        "url": rel,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def scan_assets(assets: Path) -> list[dict]:
    """Bulk-syncable assets only: the type catalogue under model/.

    Images and per-parameter styles are deliberately NOT listed. The firmware
    skips them in item_selected() and fetches an avatar per device type straight
    from /files/Assets/images/devices/<type>.svg, so publishing them here would
    be ~5 KB of entries the device reads and then ignores on every hourly pass.
    """
    if not assets.is_dir():
        return []
    items: list[dict] = []
    for path in sorted(p for p in assets.rglob("*") if p.is_file()):
        rel = path.relative_to(assets)
        if rel.parts[0] != "model":
            continue
        if rel == Path("index.json") or path.name == "device_widgets.json" or path.name == "parameters.json":
            continue
        if path.name.startswith("."):
            continue
        items.append(_asset_item(rel.as_posix(), path))
    return items


def scan_webs(webs: Path) -> list[dict]:
    if not webs.is_dir():
        return []
    items: list[dict] = []
    for ver_dir in sorted(p for p in webs.iterdir() if p.is_dir() and not p.name.startswith(".")):
        version = ver_dir.name
        # Per-version release note, carried on its own item so the device can
        # show the *new* version's notes before applying (probe_web_hint reads
        # release_note from any item whose version == latest).
        note_path = ver_dir / "release_notes.txt"
        note = note_path.read_text(encoding="utf-8", errors="replace") if note_path.is_file() else ""
        for path in sorted(p for p in ver_dir.rglob("*") if p.is_file()):
            if path.name.startswith("."):
                continue
            rel = path.relative_to(ver_dir).as_posix()
            url = f"{version}/{rel}"
            item = _item(item_id=url.replace("/", "_"), name=path.name, version=version, url=url, path=path, kind_type="web")
            if note and path.name == "release_notes.txt":
                item["release_note"] = note
            items.append(item)
    return items


def _document(kind: str, base_url: str, items: list[dict]) -> dict:
    return {
        "schema": SCHEMA,
        "kind": kind,
        "generated": datetime.now().astimezone().isoformat(timespec="seconds"),
        "base_url": base_url,
        "items": items,
    }


def build_web_index(deploy: Path) -> dict:
    """Web manifest served dynamically by store_server.py as GET /api/web."""
    return _document("web", "/files/Webs", scan_webs(deploy / "Webs"))


def build_asset_index(deploy: Path) -> dict:
    """Asset manifest served dynamically by store_server.py as GET /api/assets."""
    return _document("asset", "/files/Assets", scan_assets(deploy / "Assets"))


# --------------------------------------------------------------------------- #
# device-widget indexes (new)
# --------------------------------------------------------------------------- #
def _coerce_num(text: str):
    text = (text or "").strip()
    if not text:
        return None
    try:
        if "." in text:
            return float(text)
        return int(text)
    except ValueError:
        return text


def _icon_exists(roots: list[Path], sub: str, name: str) -> bool:
    """True when the artwork is actually shipped in one of ``roots``.

    ``roots`` are candidate ``Assets`` dirs (deploy root first, then the
    component source). Icons are optional: a parameter with no .svg, or a type
    with no avatar, must not be advertised as a URL -- the device would fetch it
    and get a 404 on every sync.
    """
    return any((r / "images" / sub / name).is_file() for r in roots)


def _device_icon_url(roots: list[Path], row: dict, device_id: str) -> str | None:
    img = (row.get("image") or "").strip()
    name = img or f"{device_id}.svg"
    if not _icon_exists(roots, "devices", name):
        return None
    return f"/files/Assets/images/devices/{name}"


def _param_icon_url(roots: list[Path], row: dict, param: str) -> str | None:
    img = (row.get("image") or "").strip()
    name = img or f"{param}.svg"
    if not _icon_exists(roots, "parameters", name):
        return None
    return f"/files/Assets/images/parameters/{name}"


def _param_meta(row: dict) -> dict:
    opts = (row.get("options") or "").strip()
    req = (row.get("requires") or "").strip()
    return {
        "label_zh": row.get("label_zh", ""),
        "label_en": row.get("label_en", ""),
        "data_type": row.get("data_type", ""),
        "min": _coerce_num(row.get("min", "")),
        "max": _coerce_num(row.get("max", "")),
        "step": _coerce_num(row.get("step", "")),
        "unit": row.get("unit", ""),
        "ui_element": row.get("ui_element", ""),
        "options": opts or None,
        "requires": req or None,
    }


def _load_csv(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def resolve_device_widget_source(deploy: Path, explicit: Path | None) -> Path | None:
    """Prefer copied data under the deploy root, else the component source."""
    local = deploy / "Assets" / "model" / "devices.csv"
    if local.is_file():
        return deploy / "Assets"
    if explicit and (explicit / "model" / "devices.csv").is_file():
        return explicit
    if (DEFAULT_DEVICE_WIDGET / "model" / "devices.csv").is_file():
        return DEFAULT_DEVICE_WIDGET
    return None


def sync_device_widget_assets(deploy: Path, src: Path) -> None:
    """Copy model/ and images/ from the component source into the deploy root.

    Used by the publish step so the deploy root is self-contained and
    re-indexable with ``--only assets``."""
    assets = deploy / "Assets"
    for sub in ("model", "images"):
        src_dir = src / sub
        if not src_dir.is_dir():
            continue
        dst_dir = assets / sub
        dst_dir.mkdir(parents=True, exist_ok=True)
        for item in src_dir.rglob("*"):
            if item.is_file():
                rel = item.relative_to(src_dir)
                target = dst_dir / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(item, target)


def generate_device_widgets(deploy: Path, source: Path) -> tuple[Path, Path]:
    devices = _load_csv(source / "model" / "devices.csv")
    params = _load_csv(source / "model" / "parameters.csv")
    # Icons are optional artwork: advertise a URL only for files that exist, in
    # the deploy root or (when publishing without a sync pass) in the source.
    icon_roots = [deploy / "Assets", source]

    param_index: dict[str, dict] = {}
    for pr in params:
        pname = (pr.get("parameter") or "").strip()
        if not pname:
            continue
        param_index[pname] = {
            "icon": _param_icon_url(icon_roots, pr, pname),
            "meta": _param_meta(pr),
        }

    device_widgets: dict[str, dict] = {}
    for dv in devices:
        did = (dv.get("device_id") or "").strip()
        if not did:
            continue
        param_names = [p.strip() for p in (dv.get("parameters") or "").split(",") if p.strip()]
        parameters: dict[str, dict] = {}
        for pname in param_names:
            if pname in param_index:
                parameters[pname] = param_index[pname]
        device_widgets[did] = {
            "icon": _device_icon_url(icon_roots, dv, did),
            "parameters": parameters,
        }

    assets = deploy / "Assets"
    assets.mkdir(parents=True, exist_ok=True)
    dw_path = assets / "device_widgets.json"
    dw_path.write_text(
        json.dumps({"schema": SCHEMA, "kind": "device_widgets", "devices": device_widgets}, indent=2, ensure_ascii=False)
        + "\n",
        encoding="utf-8",
    )
    params_path = assets / "parameters.json"
    params_path.write_text(
        json.dumps({"schema": SCHEMA, "kind": "parameters", "parameters": param_index}, indent=2, ensure_ascii=False)
        + "\n",
        encoding="utf-8",
    )

    # No per-parameter stylesheet is published. The `css` column holds a CLASS
    # NAME, not CSS text: the console hands it to dwCssClass() and the rules
    # themselves live in the bundled device_widgets.css, delivered inline via
    # /api/device_widgets. Emitting Assets/styles/<param>.css used to write a
    # bare class name into a file nobody reads. A stylesheet can still be pushed
    # by hand through the dev panel (PUT /api/dev/assets/styles/...).
    return dw_path, params_path


# --------------------------------------------------------------------------- #
# driver
# --------------------------------------------------------------------------- #
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deploy", type=Path, default=None, help="deploy root")
    parser.add_argument("--device-widget", type=Path, default=None, help="device_widget component source dir")
    parser.add_argument(
        "--only",
        choices=("all", "assets", "web"),
        default="all",
        help="regenerate subset only (assets includes device_widgets + styles)",
    )
    parser.add_argument(
        "--sync-source",
        action="store_true",
        help="copy model/ and images/ from --device-widget into deploy/Assets first",
    )
    args = parser.parse_args(argv)
    deploy = resolve_deploy_dir(args.deploy)
    deploy.mkdir(parents=True, exist_ok=True)

    if args.sync_source:
        # Always pull from the component source. resolve_device_widget_source()
        # prefers an already-copied deploy/Assets copy, which would just copy
        # deploy onto itself and silently drop any edits in the component source
        # on the 2nd+ run. --sync-source means "pull latest from source".
        src = None
        if args.device_widget and (args.device_widget / "model" / "devices.csv").is_file():
            src = args.device_widget
        elif (DEFAULT_DEVICE_WIDGET / "model" / "devices.csv").is_file():
            src = DEFAULT_DEVICE_WIDGET
        if not src:
            print("gen_deploy_index: no device_widget source found for --sync-source", file=sys.stderr)
            return 2
        sync_device_widget_assets(deploy, src)
        print(f"gen_deploy_index: synced assets from {src}")

    # Web and asset manifests are served dynamically by store_server.py
    # (GET /api/web, GET /api/assets); no static index.json is produced here.
    generated: list[Path] = []

    src = resolve_device_widget_source(deploy, args.device_widget)
    if src and args.only in ("all", "assets"):
        dw, pp = generate_device_widgets(deploy, src)
        print(f"gen_deploy_index: {dw}")
        print(f"gen_deploy_index: {pp}")
        generated += [dw, pp]
    elif args.only in ("all", "assets"):
        print("gen_deploy_index: no device_widget source (skip device_widgets.json)")

    for p in generated:
        print(f"gen_deploy_index: {p}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Export LCD device icons from minihub_device_widget SVGs.

Layout (on device after storage flash)::

    /data/ui/images/devices/<type>.png   # same stem as Web /data/images/devices/<type>.svg
    /data/ui/assets/device_icons.json    # Brookesia imageSet (id = stem)

Web keeps serving SVGs from the widget tree. LCD needs 32×32 PNG; downscaling
the full Web artwork alone is often muddy, so a few common types use purpose-
drawn glyphs and the rest are rendered from SVG via cairosvg.

Requires: Pillow; cairosvg for non-geometric types
  python3 -m pip install Pillow cairosvg

Usage:
  python3 tools/regen_lcd_device_icons.py
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "components" / "minihub_device_widget" / "images" / "devices"
DST_DIR = ROOT / "components" / "minihub_lcd" / "ui" / "images" / "devices"
ICONS_JSON = ROOT / "components" / "minihub_lcd" / "ui" / "assets" / "device_icons.json"
LEGACY_GLOB = ROOT / "components" / "minihub_lcd" / "ui" / "images" / "icon_*.png"
TRANSPARENT = (0, 0, 0, 0)


def _blank():
    from PIL import Image
    return Image.new("RGBA", (32, 32), TRANSPARENT)


def _draw_switch(im):
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((4, 3, 27, 28), radius=4, fill=(44, 44, 46, 255),
                        outline=(52, 199, 89, 255), width=2)
    d.rounded_rectangle((12, 8, 19, 23), radius=2, fill=(28, 28, 30, 255))
    d.rounded_rectangle((13, 9, 18, 15), radius=1, fill=(52, 199, 89, 255))
    d.rounded_rectangle((13, 16, 18, 22), radius=1, fill=(72, 72, 74, 255))


def _draw_light(im):
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    d.ellipse((6, 4, 25, 23), fill=(255, 214, 10, 60))
    d.ellipse((9, 6, 22, 19), fill=(255, 214, 10, 255))
    d.rectangle((13, 19, 18, 23), fill=(142, 142, 147, 255))
    d.rectangle((14, 23, 17, 26), fill=(142, 142, 147, 255))
    d.line([(12, 27), (19, 27)], fill=(142, 142, 147, 255), width=2)


def _draw_button(im):
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    d.ellipse((4, 4, 27, 27), outline=(10, 132, 255, 255), width=2)
    d.ellipse((9, 9, 22, 22), fill=(10, 132, 255, 255))
    d.ellipse((12, 12, 19, 19), fill=(44, 44, 46, 255))


def _draw_sensor(im):
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    d.arc((4, 8, 27, 31), start=200, end=340, fill=(255, 159, 10, 255), width=2)
    d.arc((8, 12, 23, 27), start=200, end=340, fill=(255, 159, 10, 200), width=2)
    d.ellipse((13, 18, 18, 23), fill=(255, 159, 10, 255))


def _draw_device(im):
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((6, 8, 25, 25), radius=3, fill=(72, 72, 74, 255),
                        outline=(174, 174, 178, 255), width=2)
    d.rectangle((10, 12, 21, 15), fill=(174, 174, 178, 255))
    d.ellipse((11, 18, 14, 21), fill=(52, 199, 89, 255))


GEOMETRIC = {
    "switch": _draw_switch,
    "light": _draw_light,
    "light_strip": _draw_light,
    "fan_light": _draw_light,
    "button": _draw_button,
    "sensor_contact": _draw_sensor,
    "sensor_motion": _draw_sensor,
    "sensor_occupancy": _draw_sensor,
    "sensor_smoke": _draw_sensor,
    "sensor_tamper": _draw_sensor,
    "sensor_water": _draw_sensor,
    "sensor_illuminance": _draw_sensor,
    "sensor_air_quality": _draw_sensor,
    "sensor_temp_humidity": _draw_sensor,
    "device": _draw_device,
}


def _from_svg(svg: Path):
    import cairosvg
    from PIL import Image
    png_bytes = cairosvg.svg2png(url=str(svg), output_width=128, output_height=128)
    im = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    return im.resize((32, 32), Image.Resampling.LANCZOS)


def main() -> int:
    try:
        from PIL import Image
    except ImportError as exc:
        print(f"Missing dependency: {exc}", file=sys.stderr)
        print("Install with: python3 -m pip install Pillow cairosvg", file=sys.stderr)
        return 1

    if not SRC.is_dir():
        print(f"SVG source missing: {SRC}", file=sys.stderr)
        return 1

    DST_DIR.mkdir(parents=True, exist_ok=True)
    stems: list[str] = []

    # Always ship a generic fallback.
    names = sorted({p.stem for p in SRC.glob("*.svg")} | {"device"})
    for stem in names:
        out = DST_DIR / f"{stem}.png"
        im = _blank()
        if stem in GEOMETRIC:
            GEOMETRIC[stem](im)
            how = "geometric"
        else:
            svg = SRC / f"{stem}.svg"
            if svg.is_file():
                try:
                    im = _from_svg(svg)
                    how = "svg"
                except Exception as exc:  # noqa: BLE001 — keep going for the rest
                    print(f"warn: {stem}: {exc}; using device glyph", file=sys.stderr)
                    _draw_device(im)
                    how = "fallback"
            else:
                _draw_device(im)
                how = "fallback"
        im.save(out, format="PNG", optimize=True)
        stems.append(stem)
        print(f"Wrote {out.relative_to(ROOT)} ({out.stat().st_size} B, {how})")

    for legacy in sorted(LEGACY_GLOB.parent.glob("icon_*.png")):
        legacy.unlink()
        print(f"Removed legacy {legacy.relative_to(ROOT)}")

    images = [
        {
            "id": stem,
            "src": f"../images/devices/{stem}.png",
            "width": 32,
            "height": 32,
        }
        for stem in stems
    ]
    ICONS_JSON.parent.mkdir(parents=True, exist_ok=True)
    ICONS_JSON.write_text(
        json.dumps({"type": "imageSet", "images": images}, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote {ICONS_JSON.relative_to(ROOT)} ({len(images)} icons)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

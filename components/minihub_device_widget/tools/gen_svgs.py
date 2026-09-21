#!/usr/bin/env python3
"""Generate device-class avatar SVGs (images/devices/).

Interactive controls are CSS widgets in minihub_web (device_widgets.css),
not static parameter mockups. Parameter preview SVGs under images/parameters/
are retired — do not regenerate them.

Usage:
    python3 tools/gen_svgs.py          # write device avatars only
    python3 tools/gen_svgs.py --png    # + PNG twins (needs cairosvg)
"""
import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(os.path.dirname(HERE), "images")
DEV_IMG = os.path.join(IMG, "devices")

XML = '<?xml version="1.0" encoding="UTF-8"?>\n'


def avatar(glyph):
    """Square 96x96 device-class icon: dark tile + accent glyph, no caption."""
    return (f'{XML}<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" '
            f'viewBox="0 0 96 96">\n'
            f'  <rect width="96" height="96" rx="20" fill="#1C1C1E"/>\n'
            f'{glyph}'
            f'</svg>\n')


# images/devices/<device>.svg - the device-class avatar.
device_svgs = {
    "light.svg": avatar(
        '  <circle cx="48" cy="42" r="26" fill="#FFD60A" opacity="0.18"/>\n'
        '  <circle cx="48" cy="42" r="17" fill="#FFD60A"/>\n'
        '  <rect x="42" y="58" width="12" height="9" rx="3" fill="#8E8E93"/>\n'
        '  <rect x="44" y="66" width="8" height="6" rx="2" fill="#8E8E93"/>\n'),
    "switch.svg": avatar(
        '  <rect x="26" y="22" width="44" height="44" rx="12" fill="#2C2C2E" '
        'stroke="#34C759" stroke-width="3"/>\n'
        '  <circle cx="40" cy="38" r="4" fill="#34C759"/>\n'
        '  <circle cx="56" cy="38" r="4" fill="#34C759"/>\n'
        '  <rect x="38" y="50" width="20" height="4" rx="2" fill="#34C759"/>\n'),
}


def render_pngs():
    try:
        import cairosvg
    except ImportError:
        print("  (cairosvg not installed - skipped PNG rendering)")
        return
    for name in device_svgs:
        src = os.path.join(DEV_IMG, name)
        dst = os.path.join(DEV_IMG, name[:-4] + ".png")
        cairosvg.svg2png(url=src, write_to=dst, output_width=200)
        print(f"  wrote {os.path.basename(dst)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--png", action="store_true",
                    help="also render PNG twins (needs cairosvg)")
    args = ap.parse_args()
    os.makedirs(DEV_IMG, exist_ok=True)
    for name, content in device_svgs.items():
        path = os.path.join(DEV_IMG, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"  wrote {os.path.relpath(path, IMG)}")
    if args.png:
        render_pngs()
    print("done (devices only; parameter mockups are CSS widgets).")


if __name__ == "__main__":
    main()

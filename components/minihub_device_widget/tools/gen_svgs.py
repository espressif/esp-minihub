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
    # Sensors: one avatar per device class (icon keyed by type — see
    # device_widgets.js dwTypeImageUrl). Keep glyphs distinct: motion =
    # walking figure + swoosh; occupancy = radar rings + person; smoke =
    # detector disc (not a cloud face); air quality = wind waves + leaf.
    "sensor_motion.svg": avatar(
        '  <path d="M22 38 Q30 28 38 38" stroke="#FF9F0A" stroke-width="3.5" '
        'fill="none" stroke-linecap="round" opacity="0.55"/>\n'
        '  <path d="M18 52 Q28 42 38 52" stroke="#FF9F0A" stroke-width="3.5" '
        'fill="none" stroke-linecap="round" opacity="0.35"/>\n'
        '  <circle cx="54" cy="26" r="8" fill="#FF9F0A"/>\n'
        '  <path d="M42 40 h18 l6 14 h-8 l-4-8 h-6 z" fill="#FF9F0A"/>\n'
        '  <path d="M48 54 l-6 22 h7 l5-14 4 14 h7 l-8-22 z" '
        'fill="#FF9F0A"/>\n'),
    "sensor_contact.svg": avatar(
        # Pair of reed/magnet blocks with a gap = door/window contact open.
        '  <rect x="18" y="26" width="22" height="44" rx="6" fill="#0A84FF"/>\n'
        '  <rect x="56" y="26" width="22" height="44" rx="6" fill="#0A84FF" '
        'opacity="0.55"/>\n'
        '  <circle cx="36" cy="48" r="4" fill="#1C1C1E"/>\n'
        '  <circle cx="60" cy="48" r="4" fill="#1C1C1E"/>\n'
        '  <path d="M42 48 H54" stroke="#0A84FF" stroke-width="3" '
        'stroke-linecap="round" stroke-dasharray="4 5"/>\n'),
    "sensor_smoke.svg": avatar(
        '  <circle cx="48" cy="54" r="26" fill="#2C2C2E" stroke="#FF453A" '
        'stroke-width="3"/>\n'
        '  <circle cx="48" cy="54" r="14" fill="none" stroke="#8E8E93" '
        'stroke-width="3"/>\n'
        '  <circle cx="48" cy="54" r="5" fill="#FF453A"/>\n'
        '  <path d="M36 26 q4-10 8-2 q4-12 8 0 q3-8 6 2" stroke="#D1D1D6" '
        'stroke-width="3.5" fill="none" stroke-linecap="round"/>\n'),
    "sensor_water.svg": avatar(
        '  <path d="M48 22 C48 22 32 40 32 52 a16 16 0 0 0 32 0 C64 40 48 22 '
        '48 22 z" fill="#64D2FF"/>\n'
        '  <circle cx="42" cy="54" r="4" fill="#1C1C1E" opacity="0.55"/>\n'
        '  <path d="M28 72 q6-5 12 0 t12 0 t12 0" stroke="#64D2FF" '
        'stroke-width="4" fill="none" stroke-linecap="round"/>\n'),
    "sensor_temp_humidity.svg": avatar(
        '  <rect x="30" y="26" width="14" height="34" rx="7" fill="#2C2C2E" '
        'stroke="#FF453A" stroke-width="3"/>\n'
        '  <circle cx="37" cy="56" r="8" fill="#FF453A"/>\n'
        '  <path d="M62 34 C62 34 50 50 50 58 a12 12 0 0 0 24 0 C74 50 62 34 '
        '62 34 z" fill="#64D2FF"/>\n'),
    "sensor_illuminance.svg": avatar(
        '  <circle cx="48" cy="46" r="15" fill="#FFD60A"/>\n'
        '  <g stroke="#FFD60A" stroke-width="4" stroke-linecap="round">\n'
        '    <line x1="48" y1="16" x2="48" y2="24"/>\n'
        '    <line x1="48" y1="68" x2="48" y2="76"/>\n'
        '    <line x1="18" y1="46" x2="26" y2="46"/>\n'
        '    <line x1="70" y1="46" x2="78" y2="46"/>\n'
        '    <line x1="27" y1="25" x2="33" y2="31"/>\n'
        '    <line x1="69" y1="25" x2="63" y2="31"/>\n'
        '    <line x1="27" y1="67" x2="33" y2="61"/>\n'
        '    <line x1="69" y1="67" x2="63" y2="61"/>\n'
        '  </g>\n'),
    "sensor_air_quality.svg": avatar(
        '  <path d="M22 34 h36" stroke="#30D158" stroke-width="5" '
        'stroke-linecap="round"/>\n'
        '  <path d="M28 48 h40" stroke="#30D158" stroke-width="5" '
        'stroke-linecap="round" opacity="0.75"/>\n'
        '  <path d="M22 62 h36" stroke="#30D158" stroke-width="5" '
        'stroke-linecap="round" opacity="0.5"/>\n'
        '  <path d="M66 28 C74 32 76 42 70 48 C78 46 80 58 72 62 C68 52 60 48 '
        '66 28 Z" fill="#30D158"/>\n'),
    # Occupancy = person inside a house (在场), distinct from motion walk.
    "sensor_occupancy.svg": avatar(
        '  <path d="M48 16 L78 40 H70 V74 H26 V40 H18 Z" fill="none" '
        'stroke="#30D158" stroke-width="3.5" stroke-linejoin="round"/>\n'
        '  <circle cx="48" cy="46" r="7" fill="#30D158"/>\n'
        '  <path d="M34 68 V56 a14 11 0 0 1 28 0 V68" fill="#30D158"/>\n'),
    # Tamper = forced-open padlock (防拆), not warning triangle / lightning.
    "sensor_tamper.svg": avatar(
        '  <path d="M34 44 V34 a14 14 0 0 1 28 0" stroke="#FF453A" '
        'stroke-width="6" fill="none" stroke-linecap="round"/>\n'
        '  <path d="M62 28 L74 18" stroke="#FF453A" stroke-width="6" '
        'fill="none" stroke-linecap="round"/>\n'
        '  <rect x="28" y="44" width="40" height="32" rx="8" fill="#FF453A"/>\n'
        '  <circle cx="48" cy="56" r="4" fill="#1C1C1E"/>\n'
        '  <rect x="46" y="56" width="4" height="12" rx="2" fill="#1C1C1E"/>\n'),
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

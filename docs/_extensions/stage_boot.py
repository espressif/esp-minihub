# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage the power-on sequence as a vertical timeline SVG.

The figure is a self-contained animated SVG (no script, no external resources)
that walks the boot stages top to bottom: each node lights in turn and the spine
segment into it flows while it is lit, so the reader sees the boot order without a
caption. It loops and stops under ``prefers-reduced-motion``.

Embedded from ``docs/_static/boot/boot_timeline.{lang}.svg`` by
``getting_started.rst``. Regenerated at build time; do not edit the staged copy.
"""

from __future__ import annotations

from pathlib import Path
from xml.sax.saxutils import escape as _esc
from minihub_figure_kit import write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "boot"

# One stage of the sweep, in seconds. The full cycle is STEP_S * len(STEPS); each
# node and its incoming spine segment share the same delay, so the light descends
# the spine in the order the firmware actually boots.
STEP_S = 1.1

ACCENT = ("#e8362d", "#1c8fb8", "#0ca678", "#c77f0a", "#7c5cff", "#d6336c", "#0a84ff")

_STEPS = {
    "en": [
        ("Firmware starts", "MiniHub starting…"),
        ("Storage mounts", "/data and /store"),
        ("Uplinks prepared", "Ethernet up; Wi-Fi STA deferred"),
        ("Web server up", "HTTP on port 80, before the radio stacks"),
        ("Adapters start", "Zigbee / BLE Mesh / … as built"),
        ("Network address", "eth got IP / wifi got IP"),
        ("Hub ready", "net · prov · web_ui · lcd_ui · eth · wifi"),
    ],
    "zh_CN": [
        ("固件启动", "MiniHub starting…"),
        ("存储挂载", "/data 与 /store"),
        ("上行就绪", "以太网已起；Wi-Fi STA 延后"),
        ("Web 服务就绪", "80 端口 HTTP，先于射频栈"),
        ("适配器启动", "Zigbee / BLE Mesh / … 按构建"),
        ("拿到地址", "eth got IP / wifi got IP"),
        ("Hub 就绪", "net · prov · web_ui · lcd_ui · eth · wifi"),
    ],
}

_PALETTE = """
:root{--ink:#14202e;--muted:#54616e;--line:rgba(20,32,46,.18);--panel:#fff;
--base:#f6f8fa}
@media (prefers-color-scheme:dark){:root{--ink:#e6edf3;--muted:#9fb0c0;
--line:rgba(230,237,243,.20);--panel:#161b22;--base:#0d1117}}
"""

_FONT = "IBM Plex Sans, Noto Sans CJK SC, sans-serif"
_MONO = "IBM Plex Mono, Menlo, monospace"


def _svg(lang: str) -> str:
    steps = _STEPS[lang]
    n = len(steps)
    cycle = STEP_S * n

    spine_x = 58
    r = 15
    top = 96
    dy = 80
    ys = [top + i * dy for i in range(n)]
    last = ys[-1]
    w = 720
    h = last + 64

    t = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" font-family="{_FONT}" role="img">',
        f"<title>MiniHub boot sequence / MiniHub 上电时序</title>",
        f"<style>{_PALETTE}"
        ".t{fill:var(--ink);font-size:15px;font-weight:700}"
        ".d{fill:var(--muted);font-size:12.5px;font-family:" + _MONO + "}"
        # one node lit at a time, sweeping down the spine
        "@keyframes spot{0%{opacity:0}1.5%{opacity:1}12%{opacity:1}"
        "14.5%{opacity:0}100%{opacity:0}}"
        "@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.12)}"
        "12%{transform:scale(1.12)}14.5%{transform:scale(1)}100%{transform:scale(1)}}"
        "@keyframes marchV{to{stroke-dashoffset:-22}}"
        "@keyframes drop{0%{transform:translateY(0);opacity:0}1.5%{opacity:1}"
        "12%{opacity:1}14.5%{transform:translateY(var(--span));opacity:0}"
        "100%{transform:translateY(var(--span));opacity:0}}"
        ".ring{fill:none;stroke-width:2;opacity:0;transform-box:fill-box;"
        "transform-origin:center}"
        ".node{fill:var(--panel);stroke:var(--line);stroke-width:2}"
        ".fill{opacity:0}"
        ".fl{fill:none;stroke-width:2.5;stroke-linecap:round;"
        "stroke-dasharray:5 7;opacity:0}"
        ".dot{opacity:0}"
        "@media (prefers-reduced-motion: reduce){.ring,.fill,.fl,.dot"
        "{animation:none}.fl{opacity:.5}.dot{display:none}"
        ".fill{opacity:1}.node{fill:var(--accent)}}"
        "</style>",
        '<defs>'
        '<marker id="bd" viewBox="0 0 10 10" refX="5" refY="8" markerWidth="6" '
        'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#1c8fb8"/>'
        '</marker>'
        '<filter id="soft" x="-20%" y="-20%" width="140%" height="140%">'
        '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" '
        'flood-opacity="0.10"/></filter>',
    ]
    for i, ac in enumerate(ACCENT[:n]):
        t.append(f'<linearGradient id="bfill{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".55"/></linearGradient>')
    t.append("</defs>")

    t.append(f'<rect width="{w}" height="{h}" fill="var(--base)"/>')
    t.append(f'<rect width="{w}" height="64" fill="var(--panel)" opacity=".55"/>')
    title = "上电时序" if lang == "zh_CN" else "Boot sequence"
    t.append(f'<text x="40" y="44" fill="var(--ink)" font-size="22" '
             f'font-weight="700">{_esc(title)}</text>')

    # base spine + flowing segments (segment i connects node i-1 to node i)
    for i in range(1, n):
        y0, y1 = ys[i - 1], ys[i]
        ac = ACCENT[i % len(ACCENT)]
        d = i * STEP_S
        t.append(f'<line x1="{spine_x}" y1="{y0}" x2="{spine_x}" y2="{y1}" '
                 f'stroke="var(--line)" stroke-width="3"/>')
        t.append(f'<line class="fl" x1="{spine_x}" y1="{y0}" x2="{spine_x}" y2="{y1}" '
                 f'stroke="{ac}" style="animation:marchV 1.1s linear infinite,'
                 f'spot {cycle:.1f}s linear infinite;animation-delay:0s,{d:.1f}s"/>')
        t.append(f'<circle class="dot" cx="{spine_x}" cy="{y0}" r="4" fill="{ac}" '
                 f'style="--span:{y1 - y0}px;animation:drop {cycle:.1f}s '
                 f'ease-in-out infinite;animation-delay:{d:.1f}s"/>')

    # nodes
    for i, (head, detail) in enumerate(steps):
        ac = ACCENT[i % len(ACCENT)]
        y = ys[i]
        d = i * STEP_S
        t.append(f'<circle cx="{spine_x}" cy="{y}" r="{r}" class="node"/>')
        t.append(f'<circle class="fill" cx="{spine_x}" cy="{y}" r="{r - 4}" '
                 f'fill="url(#bfill{i})" style="animation:spot {cycle:.1f}s '
                 f'linear infinite;animation-delay:{d:.1f}s"/>')
        t.append(f'<circle class="ring" cx="{spine_x}" cy="{y}" r="{r + 6}" '
                 f'stroke="{ac}" style="animation:spot {cycle:.1f}s linear infinite,'
                 f'breath {cycle:.1f}s ease-in-out infinite;'
                 f'animation-delay:{d:.1f}s,{d:.1f}s"/>')
        t.append(f'<text x="92" y="{y - 3}" class="t">{_esc(head)}</text>')
        t.append(f'<text x="92" y="{y + 16}" class="d">{_esc(detail)}</text>')

    t.append("</svg>")
    return "\n".join(t)


def stage_boot_diagram() -> Path:
    """Write the boot-sequence timeline for each language."""
    _STAGED.mkdir(parents=True, exist_ok=True)
    for lang in ("en", "zh_CN"):
        write_text(_STAGED / f"boot_timeline.{lang}.svg", _svg(lang))
    # Drop a bare basename so Sphinx does not have to disambiguate two languages.
    (_STAGED / "boot_timeline.svg").unlink(missing_ok=True)
    return _STAGED

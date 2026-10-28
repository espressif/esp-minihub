# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage the landing-page hero banner as a self-contained animated SVG.

A wide banner with the product wordmark, a one-line tagline, and a small
data-flow pulse along the bottom — the same accent palette and timed walkthrough
as the flow figures, so the home page reads as the same product. No script, no
external resources; it stops under ``prefers-reduced-motion``.

Embedded from ``docs/_static/hero/hero.{lang}.svg`` by ``index.rst``.
Regenerated at build time; do not edit the staged copy.
"""

from __future__ import annotations

from pathlib import Path
from xml.sax.saxutils import escape as _esc
from minihub_figure_kit import write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "hero"

_STEP_S = 1.3
_ACCENT = ("#e8362d", "#1c8fb8", "#0ca678", "#c77f0a", "#7c5cff")

_COPY = {
    "en": (
        "Host firmware for a multi-protocol device hub",
        "One image · web console · REST · WebSocket · rules engine · adapters",
    ),
    "zh_CN": (
        "把多种协议设备聚成一台 Hub 的 Host 固件",
        "一个固件 · Web 控制台 · REST · WebSocket · 规则引擎 · 适配器",
    ),
}


def _svg(lang: str) -> str:
    tagline, sub = _COPY[lang]
    w, h = 1180, 260
    cycle = _STEP_S * len(_ACCENT)
    x0, x1 = 56, 1124
    ty = 224  # bottom pulse track

    t = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" '
        f'font-family="IBM Plex Sans, Noto Sans CJK SC, sans-serif" role="img">',
        "<title>MiniHub — host firmware for a multi-protocol device hub</title>",
        "<style>"
        ":root{--ink:#f5f7fa;--mut:rgba(245,247,250,.72);--dim:rgba(245,247,250,.5);"
        "--line:rgba(245,247,250,.18)}"
        ".t{fill:var(--ink);font-size:54px;font-weight:800;letter-spacing:-.5px}"
        ".g{fill:var(--mut);font-size:20px;font-weight:500}"
        ".s{fill:var(--dim);font-size:14px}"
        "@keyframes spot{0%{opacity:0}1.5%{opacity:1}12%{opacity:1}14.5%{opacity:0}"
        "100%{opacity:0}}"
        "@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.18)}"
        "12%{transform:scale(1.18)}14.5%{transform:scale(1)}100%{transform:scale(1)}}"
        "@keyframes march{to{stroke-dashoffset:-22}}"
        "@keyframes travel{0%{transform:translateX(0);opacity:0}1.5%{opacity:1}"
        "12%{opacity:1}14.5%{transform:translateX(var(--span));opacity:0}"
        "100%{transform:translateX(var(--span));opacity:0}}"
        ".node{opacity:0}"
        ".fl{fill:none;stroke-width:2.5;stroke-linecap:round;stroke-dasharray:5 7;"
        "opacity:0}"
        ".dot{opacity:0}"
        "@media (prefers-reduced-motion: reduce){.node,.fl,.dot{animation:none}"
        ".node{opacity:1}.dot{display:none}.fl{opacity:.45}}"
        "</style>",
        "<defs>"
        '<linearGradient id="hb" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#101a2e"/>'
        '<stop offset="1" stop-color="#0b1020"/></linearGradient>'
        '<radialGradient id="hg" cx="0.82" cy="0.18" r="0.7">'
        '<stop offset="0" stop-color="#1c8fb8" stop-opacity=".35"/>'
        '<stop offset="1" stop-color="#1c8fb8" stop-opacity="0"/></radialGradient>'
        '<filter id="soft" x="-20%" y="-20%" width="140%" height="140%">'
        '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#000" '
        'flood-opacity="0.25"/></filter>'
        "</defs>",
        f'<rect width="{w}" height="{h}" rx="20" fill="url(#hb)"/>',
        f'<rect width="{w}" height="{h}" rx="20" fill="url(#hg)"/>',
        f'<text x="56" y="108" class="t">MiniHub</text>',
        f'<text x="58" y="146" class="g">{_esc(tagline)}</text>',
        f'<text x="58" y="176" class="s">{_esc(sub)}</text>',
    ]

    # bottom pulse track: nodes light in sequence, connectors march
    span = (x1 - x0) / (len(_ACCENT) - 1)
    for i in range(len(_ACCENT) - 1):
        xa = x0 + i * span
        xb = x0 + (i + 1) * span
        ac = _ACCENT[i % len(_ACCENT)]
        d = i * _STEP_S
        t.append(f'<line x1="{xa:.0f}" y1="{ty}" x2="{xb:.0f}" y2="{ty}" '
                 f'stroke="var(--line)" stroke-width="2.5"/>')
        t.append(f'<line class="fl" x1="{xa:.0f}" y1="{ty}" x2="{xb:.0f}" y2="{ty}" '
                 f'stroke="{ac}" style="animation:march 1.1s linear infinite,'
                 f'spot {cycle:.1f}s linear infinite;animation-delay:0s,{d:.1f}s"/>')
        t.append(f'<circle class="dot" cx="{xa:.0f}" cy="{ty}" r="4" fill="{ac}" '
                 f'style="--span:{span:.0f}px;animation:travel {cycle:.1f}s '
                 f'ease-in-out infinite;animation-delay:{d:.1f}s"/>')
    for i, ac in enumerate(_ACCENT):
        cx = x0 + i * span
        d = i * _STEP_S
        t.append(f'<circle cx="{cx:.0f}" cy="{ty}" r="8" fill="var(--line)"/>')
        t.append(f'<circle class="node" cx="{cx:.0f}" cy="{ty}" r="6" fill="{ac}" '
                 f'style="animation:spot {cycle:.1f}s linear infinite,'
                 f'breath {cycle:.1f}s ease-in-out infinite;'
                 f'animation-delay:{d:.1f}s,{d:.1f}s"/>')

    t.append("</svg>")
    return "\n".join(t)


def stage_hero_banner() -> Path:
    """Write the hero banner for each language."""
    _STAGED.mkdir(parents=True, exist_ok=True)
    for lang in ("en", "zh_CN"):
        write_text(_STAGED / f"hero.{lang}.svg", _svg(lang))
    (_STAGED / "hero.svg").unlink(missing_ok=True)
    return _STAGED

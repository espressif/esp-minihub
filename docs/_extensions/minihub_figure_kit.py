# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Shared layout kit for the guide's flow figures.

One table per figure, rendered twice: a self-contained animated SVG for the page
body and a standalone interactive page for the "Interactive version" link. Both
come from the same stage list, so figure and page cannot drift apart.

All motion is CSS (no scripts in the SVG, no external resources), and it is
turned off under ``prefers-reduced-motion``. The SVG carries ``width``/``height``
as well as ``viewBox``: without an intrinsic size an ``<img>`` falls back to a
default box, the figure's own aspect ratio is lost and the drawing ends up
letterboxed inside it.
"""

from __future__ import annotations

import unicodedata
from pathlib import Path
from xml.sax.saxutils import escape as _esc

FIG_W = 1180
MARGIN = 40
GAP = 22
CARD_TOP = 168
PAD = 14
STEP_S = 1.3

ACCENT = ("#e8362d", "#1c8fb8", "#0ca678", "#c77f0a", "#7c5cff", "#d6336c")

CSS = """
:root{--ink:#14202e;--muted:#54616e;--line:rgba(20,32,46,.16);--panel:#fff;
--base:#f6f8fa;--red:#e8362d;--cyan:#1c8fb8;--viz:#0ca678;--amber:#c77f0a}
@media (prefers-color-scheme:dark){:root{--ink:#e6edf3;--muted:#9fb0c0;
--line:rgba(230,237,243,.18);--panel:#161b22;--base:#0d1117}}
"""

KIN = """@keyframes spot{0%{opacity:0}1.5%{opacity:.85}12%{opacity:.85}
14.5%{opacity:0}100%{opacity:0}}
@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.014)}
12%{transform:scale(1.014)}14.5%{transform:scale(1)}100%{transform:scale(1)}}
@keyframes bar{0%{opacity:.4}1.5%{opacity:1}12%{opacity:1}
14.5%{opacity:.4}100%{opacity:.4}}
@keyframes march{to{stroke-dashoffset:-24}}
@keyframes marchBack{to{stroke-dashoffset:24}}
@keyframes travel{0%{transform:translateX(0);opacity:0}1.5%{opacity:1}
12%{opacity:1}14.5%{transform:translateX(var(--span));opacity:0}
100%{transform:translateX(var(--span));opacity:0}}
@keyframes travelBack{0%{transform:translateX(0);opacity:0}1.5%{opacity:1}
12%{opacity:1}14.5%{transform:translateX(calc(-1*var(--span)));opacity:0}
100%{transform:translateX(calc(-1*var(--span)));opacity:0}}
.ring{fill:none;stroke-width:2;opacity:0;transform-box:fill-box;
transform-origin:center}
.halo{fill:none;stroke-width:7;opacity:0}
.bar{opacity:.4}.wash{opacity:0}
.fl{fill:none;stroke-width:2;stroke-linecap:round;stroke-dasharray:5 7;
opacity:0}
.dot{opacity:0}
@media (prefers-reduced-motion: reduce){.ring,.halo,.bar,.wash,.fl,.dot
{animation:none}.fl{opacity:.4}.dot{display:none}.bar{opacity:.75}
.ring,.halo,.wash{opacity:0}}
"""


def text_width(s: str, size: float) -> float:
    """Rough rendered width: CJK glyphs take a full em, latin about 0.55."""
    return sum(size * (1.0 if unicodedata.east_asian_width(c) in ("W", "F") else 0.55)
               for c in s)


def write_text(path: Path, text: str) -> Path:
    """Write a staged artifact, always ending it with a newline.

    Generators assemble their output by joining lines, which drops the final
    newline. When the result is a tracked file, that makes every docs build
    rewrite it and ``git status`` can no longer tell a real change from a build.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
    return path


def wrap_words(s: str, size: float, maxw: float, limit: int = 4) -> list[str]:
    """Greedy word wrap that also breaks CJK runs and over-long identifiers.

    Chinese has no spaces, so a whole note arrives as one "word"; it must still
    be cut to the card width, at any glyph. Latin names break after separators.
    """
    out: list[str] = []
    cur = ""
    for w in s.split():
        if text_width(w, size) > maxw:
            if cur:
                out.append(cur)
                cur = ""
            for part in _split_long(w, size, maxw):
                cand = cur + part if cur else part
                if text_width(cand, size) <= maxw or not cur:
                    cur = cand
                else:
                    out.append(cur)
                    cur = part
            continue
        cand = w if not cur else cur + " " + w
        if text_width(cand, size) <= maxw or not cur:
            cur = cand
        else:
            out.append(cur)
            cur = w
    if cur:
        out.append(cur)
    return out[:limit]


CLOSE_PUNCT = "，。、；：！？）】》%）"
OPEN_PUNCT = "（【《"


def _split_long(w: str, size: float, maxw: float) -> list[str]:
    """Cut one over-long token into pieces that each fit *maxw*.

    Latin identifiers break at their own separators (``_``, ``/``, ``-``, ``.``),
    never mid-syllable: ``set_boot_part`` / ``ition`` reads as a different symbol.
    Chinese has no spaces, so it breaks at any glyph — with the typographic rule
    that a line may not end on an opening bracket or start on a closing one.
    """
    parts: list[str] = []
    cur = ""
    for ch in w:
        nxt = cur + ch
        if cur and text_width(nxt, size) > maxw:
            parts.append(cur)
            cur = ch
            continue
        cur = nxt
        if ch in "/,;:()" and text_width(cur, size) > maxw * 0.45:
            parts.append(cur)
            cur = ""
        elif cur and ch in "_-.":
            parts.append(cur)
            cur = ""
    if cur:
        parts.append(cur)
    return _fix_orphans(parts)


def _fix_orphans(parts: list[str]) -> list[str]:
    """Move stranded punctuation onto the line it belongs to."""
    out = list(parts)
    for i in range(1, len(out)):
        while out[i] and out[i][0] in CLOSE_PUNCT:
            out[i - 1] += out[i][0]
            out[i] = out[i][1:]
        while out[i - 1] and out[i - 1][-1] in OPEN_PUNCT and out[i]:
            out[i - 1] += out[i][0]
            out[i] = out[i][1:]
    return [p for p in out if p]


def _card_lines(stage, card_w, lang="zh"):
    """One card's text lines.

    A stage is ``(code, label_zh, actor, note_zh, note_en)`` — ``note_en`` may be
    omitted when the note is already English. ``code`` is an identifier (a
    function name, a route), so both languages show it; ``label_zh`` is prose, so
    the English build drops it. ``actor`` is usually an identifier too; when it
    reads as prose, pass ``(actor_zh, actor_en)``.
    """
    key_en, key_zh, actor, note = stage[0], stage[1], stage[2], stage[3]
    note_en = stage[4] if len(stage) > 4 else note
    if lang == "en":
        key_zh = ""
        note = note_en
    if isinstance(actor, tuple):
        actor = actor[0] if lang != "en" else actor[1]
    inner = card_w - 2 * PAD
    lh = dict(t=19, tz=17, ac=16, nt=15)
    size = dict(t=16, tz=15, ac=13, nt=12.5)
    # A route or a function name reads better one size smaller than cut in half,
    # so shrink the title until it fits instead of wrapping it at 16 px.
    maxw = inner
    while size["t"] > 12.5 and text_width(key_en, size["t"]) > maxw:
        size["t"] -= 0.5
    # 1.35 em, not 1.19: the title box carries a descender (p, g) and the line
    # under it is CJK, whose ink starts high — at 1.19 the two boxes overlap.
    lh["t"] = max(19, int(round(size["t"] * 1.35)))
    le = wrap_words(key_en, size["t"], maxw, 3)
    lz = wrap_words(key_zh, size["tz"], maxw, 2) if key_zh else []
    la = wrap_words(actor, size["ac"], inner, 2)
    ln = wrap_words(note, size["nt"], inner, 5)
    height = (34 + len(le) * lh["t"] + len(lz) * lh["tz"] + 8 + len(la) * lh["ac"]
              + 8 + len(ln) * lh["nt"] + 10)
    return le, lz, la, ln, height, size, lh


def svg_figure(title: str, subtitle: str, lead: str, stages,
               legend: tuple | list = (), *, return_from: int | None = None,
               return_to: int = 0, return_caption: str = "",
               cycle_s: float | None = None, lang: str = "zh") -> str:
    """Forward card row, optional return lane, then the legend lines.

    ``return_from`` is the index whose work loops back to ``return_to``; the
    return lane is drawn under the whole row, which is how a failed boot rolling
    back (or a missing credential going to fetch one) reads as "this round comes
    around again here".
    """
    n = len(stages)
    inner_w = FIG_W - 2 * MARGIN
    card_w = (inner_w - (n - 1) * GAP) / n
    spec = [_card_lines(s, card_w, lang) for s in stages]
    card_h = max(s[4] for s in spec)
    card_b = CARD_TOP + card_h
    y_ret = card_b + 44 if return_from is not None else card_b + 8
    h = y_ret + (66 if return_from is not None else 12) + 22 * len(legend)
    cycle = cycle_s if cycle_s else STEP_S * n
    yc = CARD_TOP + 30

    t = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{FIG_W}" height="{h:.0f}" '
         f'viewBox="0 0 {FIG_W} {h:.0f}" '
         f'font-family="IBM Plex Sans, Noto Sans CJK SC, sans-serif" role="img">',
         f"<title>{_esc(title)}</title>",
         f"<desc>{_esc(lead)}</desc>",
         f"<style>{CSS}{KIN}</style>",
         "<defs>"
         '<marker id="fk-back" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6"'
         ' markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#1c8fb8"/></marker>'
         '<filter id="fk-soft" x="-20%" y="-20%" width="140%" height="140%">'
         '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" flood-opacity="0.10"/></filter>']
    for i in range(n):
        ac = ACCENT[i % len(ACCENT)]
        t.append(f'<linearGradient id="fk-wash{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}" stop-opacity=".12"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".02"/></linearGradient>')
        t.append(f'<linearGradient id="fk-bar{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".55"/></linearGradient>')
    t.append("</defs>")
    t.append(f'<rect width="{FIG_W}" height="{h:.0f}" fill="var(--base)"/>')
    t.append(f'<rect width="{FIG_W}" height="{CARD_TOP - 30}" fill="var(--panel)" opacity=".55"/>')
    sub = (f'<tspan style="fill:var(--muted)" font-weight="500" dx="14" '
           f'font-size="15">{_esc(subtitle)}</tspan>' if subtitle else '')
    t.append(f'<text x="{MARGIN}" y="46" fill="var(--ink)" font-size="23" font-weight="700">'
             f'{_esc(title)}{sub}</text>')
    t.append(f'<text x="{MARGIN}" y="74" style="fill:var(--muted)" font-size="13.5">{_esc(lead)}</text>')
    if return_caption:
        t.append(f'<text x="{MARGIN}" y="98" style="fill:var(--muted)" font-size="12.5">'
                 f'{_esc(return_caption)}</text>')

    for i in range(n - 1):
        x0 = MARGIN + i * (card_w + GAP) + card_w
        x1 = MARGIN + (i + 1) * (card_w + GAP)
        ac = ACCENT[i % len(ACCENT)]
        d = i * STEP_S
        t.append(f'<path d="M{x0:.0f} {yc} H{x1:.0f}" stroke="var(--line)" stroke-width="2" fill="none"/>')
        t.append(f'<path class="fl" d="M{x0:.0f} {yc} H{x1:.0f}" stroke="{ac}" '
                 f'style="animation:march 1.1s linear infinite,spot {cycle:.1f}s linear infinite;'
                 f'animation-delay:0s,{d:.1f}s"/>')
        t.append(f'<circle class="dot" cx="{x0:.0f}" cy="{yc}" r="3.5" fill="{ac}" '
                 f'style="--span:{GAP:.0f}px;animation:travel {cycle:.1f}s ease-in-out infinite;'
                 f'animation-delay:{d:.1f}s"/>')

    if return_from is not None:
        x_from = MARGIN + return_from * (card_w + GAP) + card_w / 2
        x_to = MARGIN + card_w / 2
        d = return_from * STEP_S
        t.append(f'<path d="M{x_from:.0f} {y_ret} H{x_to:.0f}" stroke="var(--cyan)" stroke-width="2" '
                 f'stroke-dasharray="5 4" fill="none" opacity=".45" marker-end="url(#fk-back)"/>')
        t.append(f'<path class="fl" d="M{x_from:.0f} {y_ret} H{x_to:.0f}" stroke="var(--cyan)" '
                 f'style="animation:marchBack 1.1s linear infinite,spot {cycle:.1f}s linear infinite;'
                 f'animation-delay:0s,{d:.1f}s"/>')
        t.append(f'<circle class="dot" cx="{x_from:.0f}" cy="{y_ret}" r="3.5" fill="var(--cyan)" '
                 f'style="--span:{x_from - x_to:.0f}px;animation:travelBack {cycle:.1f}s ease-in-out infinite;'
                 f'animation-delay:{d:.1f}s"/>')

    for i, (le, lz, la, ln, _hgt, size, lh) in enumerate(spec):
        x = MARGIN + i * (card_w + GAP)
        ac = ACCENT[i % len(ACCENT)]
        d = i * STEP_S
        t.append('<g filter="url(#fk-soft)">')
        t.append(f'<rect x="{x:.0f}" y="{CARD_TOP}" width="{card_w:.0f}" height="{card_h:.0f}" rx="10" '
                 f'fill="var(--panel)" stroke="var(--line)"/>')
        t.append(f'<rect class="wash" x="{x:.0f}" y="{CARD_TOP}" width="{card_w:.0f}" height="{card_h:.0f}" '
                 f'rx="10" fill="url(#fk-wash{i})" style="animation:spot {cycle:.1f}s linear infinite;'
                 f'animation-delay:{d:.1f}s"/></g>')
        t.append(f'<rect class="bar" x="{x:.0f}" y="{CARD_TOP + 14:.0f}" width="3" height="{card_h - 28:.0f}" '
                 f'rx="1.5" fill="url(#fk-bar{i})" style="animation:bar {cycle:.1f}s linear infinite;'
                 f'animation-delay:{d:.1f}s"/>')
        t.append(f'<rect class="halo" x="{x - 5:.0f}" y="{CARD_TOP - 5:.0f}" width="{card_w + 10:.0f}" '
                 f'height="{card_h + 10:.0f}" rx="16" stroke="{ac}" style="animation:spot {cycle:.1f}s '
                 f'linear infinite;animation-delay:{d:.1f}s"/>')
        t.append(f'<rect class="ring" x="{x - 3:.0f}" y="{CARD_TOP - 3:.0f}" width="{card_w + 6:.0f}" '
                 f'height="{card_h + 6:.0f}" rx="13" stroke="{ac}" style="animation:spot {cycle:.1f}s '
                 f'linear infinite,breath {cycle:.1f}s ease-in-out infinite;animation-delay:{d:.1f}s,{d:.1f}s"/>')
        tx = x + PAD
        ly = CARD_TOP + 22
        for seg in le:
            t.append(f'<text x="{tx:.0f}" y="{ly:.0f}" fill="var(--ink)" font-size="{size["t"]}" '
                     f'font-weight="600">{_esc(seg)}</text>')
            ly += lh["t"]
        tx = x + PAD
        for seg in lz:
            t.append(f'<text x="{tx:.0f}" y="{ly:.0f}" fill="var(--muted)" font-size="{size["tz"]}">'
                     f'{_esc(seg)}</text>')
            ly += lh["tz"]
        ly += 6
        for seg in la:
            t.append(f'<text x="{tx:.0f}" y="{ly:.0f}" fill="var(--amber)" font-size="{size["ac"]}" '
                     f'font-weight="600">{_esc(seg)}</text>')
            ly += lh["ac"]
        ly += 4
        for seg in ln:
            t.append(f'<text x="{tx:.0f}" y="{ly:.0f}" fill="var(--muted)" font-size="{size["nt"]}">'
                     f'{_esc(seg)}</text>')
            ly += lh["nt"]

    y_foot = y_ret + 28 if return_from is not None else card_b + 34
    for k, line in enumerate(legend):
        t.append(f'<text x="{MARGIN}" y="{y_foot + 22 * k:.0f}" style="fill:var(--muted)" '
                 f'font-size="13.5">{_esc(line)}</text>')
    t.append("</svg>")
    return "\n".join(t)


def grid_figure(title: str, subtitle: str, lead: str, lanes, legend=(),
                *, cycle_s: float = 6.0, lang: str = "zh") -> str:
    """Lane grid: one row per job, small cards for the things inside that job.

    ``lanes`` is a list of ``(lane_label_en, lane_label_zh, [(name, gloss_en,
    gloss_zh), ...])``. A lane lights up in turn. The rows group by job and claim
    no dependency order — the real edges run both ways — so nothing here draws an
    arrow between them.
    """
    n = len(lanes)
    label_w, card_gap, per_row, card_h = 168, 10, 6, 46
    inner = FIG_W - 2 * MARGIN
    card_area = inner - label_w - 40
    lane_pad = 8
    rows_per_lane = [max(1, -(-len(cards) // per_row)) for _e, _z, cards in lanes]
    lane_h = 22 + max(rows_per_lane) * card_h + (max(rows_per_lane) - 1) * 6
    top = CARD_TOP
    h = top + n * (lane_h + lane_pad) + 26 + 22 * len(legend)
    t = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{FIG_W}" height="{h:.0f}" '
         f'viewBox="0 0 {FIG_W} {h:.0f}" '
         f'font-family="IBM Plex Sans, Noto Sans CJK SC, sans-serif" role="img">',
         f"<title>{_esc(title)}</title>", f"<desc>{_esc(lead)}</desc>",
         f"<style>{CSS}{KIN}</style>",
         '<defs>',
         '<filter id="fk-soft" x="-20%" y="-20%" width="140%" height="140%">'
         '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" '
         'flood-opacity="0.10"/></filter>']
    for i in range(n):
        ac = ACCENT[i % len(ACCENT)]
        t.append(f'<linearGradient id="fg-wash{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}" stop-opacity=".10"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".02"/></linearGradient>')
    t.append("</defs>")
    t.append(f'<rect width="{FIG_W}" height="{h:.0f}" fill="var(--base)"/>')
    t.append(f'<rect width="{FIG_W}" height="{top - 30}" fill="var(--panel)" opacity=".55"/>')
    sub = (f'<tspan style="fill:var(--muted)" font-weight="500" dx="14" '
           f'font-size="15">{_esc(subtitle)}</tspan>' if subtitle else '')
    t.append(f'<text x="{MARGIN}" y="46" fill="var(--ink)" font-size="23" font-weight="700">'
             f'{_esc(title)}{sub}</text>')
    t.append(f'<text x="{MARGIN}" y="74" style="fill:var(--muted)" font-size="13.5">'
             f'{_esc(lead)}</text>')

    for i, (lab_en, lab_zh, cards) in enumerate(lanes):
        y = top + i * (lane_h + lane_pad)
        ac = ACCENT[i % len(ACCENT)]
        d = i * (cycle_s / n)
        t.append('<g filter="url(#fk-soft)">')
        t.append(f'<rect x="{MARGIN + 22}" y="{y}" width="{inner - 22}" height="{lane_h}" '
                 'rx="10" fill="var(--panel)" stroke="var(--line)"/>')
        t.append(f'<rect class="wash" x="{MARGIN + 22}" y="{y}" width="{inner - 22}" '
                 f'height="{lane_h}" rx="10" fill="url(#fg-wash{i})" '
                 f'style="animation:spot {cycle_s:.1f}s linear infinite;animation-delay:{d:.2f}s"/></g>')
        t.append(f'<rect class="bar" x="{MARGIN + 22}" y="{y + 10}" width="3" height="{lane_h - 20}" '
                 f'rx="1.5" fill="{ac}" style="animation:bar {cycle_s:.1f}s linear infinite;'
                 f'animation-delay:{d:.2f}s"/>')
        t.append(f'<text x="{MARGIN + 36}" y="{y + 28}" fill="var(--ink)" font-size="14.5" '
                 f'font-weight="700">{_esc(lab_en if lang == "en" else lab_zh)}</text>')
        cols = min(len(cards), per_row)
        cw = (card_area - (cols - 1) * card_gap) / cols
        x0 = MARGIN + 22 + label_w
        for k, card in enumerate(cards):
            name, ge, gz = card if isinstance(card, tuple) else (card, "", "")
            row_i, col_i = divmod(k, per_row)
            x = x0 + col_i * (cw + card_gap)
            cy = y + 14 + row_i * (card_h + 6)
            t.append(f'<rect x="{x:.0f}" y="{cy:.0f}" width="{cw:.0f}" height="{card_h}" '
                     'rx="7" fill="var(--base)" stroke="var(--line)"/>')
            ty = cy + 17
            for seg in wrap_words(name, 12, cw - 12, 2):
                t.append(f'<text x="{x + 6:.0f}" y="{ty:.0f}" fill="var(--ink)" font-size="12" '
                         f'font-weight="600" style="font-family:IBM Plex Mono,Menlo,monospace">'
                         f'{_esc(seg)}</text>')
                ty += 14
            gloss = ge if lang == "en" else gz
            for seg in wrap_words(gloss, 11, cw - 12, 1):
                t.append(f'<text x="{x + 6:.0f}" y="{ty + 1:.0f}" fill="var(--muted)" '
                         f'font-size="11">{_esc(seg)}</text>')
    y_foot = top + n * (lane_h + lane_pad) + 18
    for k, line in enumerate(legend):
        t.append(f'<text x="{MARGIN}" y="{y_foot + 22 * k:.0f}" style="fill:var(--muted)" '
                 f'font-size="13.5">{_esc(line)}</text>')
    t.append("</svg>")
    return "\n".join(t)


def interactive_page(title: str, lead: str, stages, note: str,
                     lang: str = "zh") -> str:
    """The standalone tour page behind the "Interactive version" link.

    Same stage table as the inline figure (the tuple is described in
    ``_card_lines``), no SVG: cards advance on a timer, pause on hover or focus,
    and do not move at all under prefers-reduced-motion. Self-contained — no
    script or font is fetched, so the offline note has its own paragraph and
    stays visible while the tour text changes.
    """
    def _card(i, st):
        label = "" if lang == "en" else " <span>" + _esc(st[1]) + "</span>"
        actor = st[2][0] if isinstance(st[2], tuple) else st[2]
        if lang == "en" and isinstance(st[2], tuple):
            actor = st[2][1]
        note_i = st[4] if (lang == "en" and len(st) > 4) else st[3]
        return ('<article class="step" data-i="%d"><h3>%s%s</h3>'
                '<p class="actor">%s</p><p class="note">%s</p></article>'
                % (i, _esc(st[0]), label, _esc(actor), _esc(note_i)))

    cards = "".join(_card(i, st) for i, st in enumerate(stages))
    step_ms = int(STEP_S * 1000)
    html_lang = "en" if lang == "en" else "zh-CN"
    head = ('<!doctype html><html lang="' + html_lang + '"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>{_esc(title)}</title><style>' + CSS + KIN + """
body{margin:0;padding:28px;background:var(--base);color:var(--ink);
font:16.5px/1.65 IBM Plex Sans,"Noto Sans CJK SC",sans-serif}
h1{font-size:25px;margin:0 0 6px}
p.lead{color:var(--muted);margin:0 0 14px;font-size:14.5px;max-width:80ch}
.tour{height:3px;border-radius:2px;background:var(--line);overflow:hidden;margin:0 0 22px}
.tour>i{display:block;height:100%;width:0;
background:linear-gradient(90deg,var(--red),var(--amber),var(--cyan));
animation:fill linear infinite}
@keyframes fill{from{width:0}to{width:100%}}
.steps{display:flex;flex-wrap:wrap;gap:12px}
.step{border:1px solid var(--line);border-radius:12px;background:var(--panel);
padding:14px 16px;margin:0;flex:1 1 280px;
transition:box-shadow .25s,transform .25s,border-color .25s}
.step.on{border-color:var(--red);
box-shadow:0 0 0 2px var(--red) inset,0 6px 18px rgba(20,32,46,.08);
transform:translateY(-3px)}
.step h3{margin:0 0 8px;font-size:16px}
.step h3 span{color:var(--muted);font-weight:400;font-size:15px;margin-left:8px}
.actor{color:var(--amber);font-size:13.5px;font-weight:600;margin:0 0 8px}
.note{color:var(--muted);font-size:13.5px;margin:0}
@media (max-width:720px){.step{flex:1 1 100%}}
@media (prefers-reduced-motion: reduce){.tour>i{animation:none;width:100%;opacity:.3}
.step{transition:none}}
""")
    body = ('</style></head><body>'
            f'\n<h1>{_esc(title)}</h1>\n<p class="lead">{_esc(lead)}</p>\n'
            f'<div class="tour"><i id="bar" style="animation-duration:{step_ms * len(stages)}ms"></i></div>\n'
            f'<main class="steps" id="steps">{cards}</main>\n'
            '<p id="say" style="margin-top:18px;color:var(--ink);font-size:14px"></p>\n'
            f'<p style="margin-top:6px;color:var(--muted);font-size:14px">{_esc(note)}</p>\n')
    js = """
<script>
(function(){
  var steps=[].slice.call(document.querySelectorAll(".step"));
  var say=document.getElementById("say");
  var i=0,timer=null;
  var still=window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  function show(k){
    steps.forEach(function(s,j){s.classList.toggle("on",j===k)});
    say.textContent=(k+1)+". "+steps[k].querySelector("h3").textContent;
  }
  function play(){ if(still) return;
    timer=setInterval(function(){i=(i+1)%steps.length;show(i)},__STEP__); }
  function pause(){ if(timer){clearInterval(timer);timer=null;} }
  show(0);
  if(!still){
    play();
    var m=document.getElementById("steps");
    m.addEventListener("mouseenter",pause);
    m.addEventListener("mouseleave",play);
    m.addEventListener("focusin",pause);
  }
})();
</script></body></html>
"""
    return (head + body + js).replace("__STEP__", str(step_ms))

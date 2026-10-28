# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage the end-to-end data-flow figure: static SVG plus an interactive page.

Sphinx ``html_static_path`` ships ``_static/data_flow/``. The figure is generated
from the single ``STAGES`` table below, so the picture, the interactive page and
the captions cannot drift apart. Do not edit staged copies.

Keep the data honest: every label here must be true of the code it names, and L3
rows are dropped for the ``hide_l3`` documentation build.

Motion design (why it looks the way it does): the figure is a *timed
walkthrough*, not decoration. Stage ``i``, the connector into it, and the comet
on that connector all share one ``cycle`` (1.2 s per stage) with per-stage
delays, so the picture tours the loop in the same order the prose does. The
cyan return path lights when the consumers stage is active — that is the rule
action starting the next round. Everything is CSS (transform / opacity /
stroke-dashoffset), so it survives ``<img>`` embedding, needs no JavaScript,
and stops under ``prefers-reduced-motion``.
"""

from __future__ import annotations

from pathlib import Path
from xml.sax.saxutils import escape as _esc
from minihub_figure_kit import text_width as _tw, wrap_words as _wrap_words, write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "data_flow"

# (en, zh, actor, note) — actor is a component/route label, note is the one-line
# behaviour. The diagram reads left-to-right: one control round trip. The cyan
# dashed return shows a rule action starting the next round.
STAGES: list[tuple] = [
    ("Control", "控制", "UI → REST → adapter",
     "A tap posts /api/devices/control; the adapter drives it"),
    ("Device acts", "设备执行", "Adapter → radio",
     "The adapter sends the command over its radio; the device acts"),
    ("Point update", "点位更新", "Adapter → Core point",
     "The new value lands in the point's props.params"),
    ("Event bus", "事件总线", "point.updated",
     "A changed value is broadcast as point.updated"),
    ("Consumers", "消费者", "WS UI + rules",
     "The socket re-fetches and redraws; rules evaluate and act"),
    ("Store modules", "商店 模块", "L1 / L2 / L3",
     "Modules subscribe to the same bus and extend the Host"),
]

# The cyan dashed return runs from this stage (0-based) back to stage 0: a rule
# action issues a command, and that command starts the next control round.
RET_FROM = 4

# One stage of the timed walkthrough, in seconds. The full cycle is
# ``STEP_S * len(STAGES)``; every animation below shares that cycle and is
# offset by ``i * STEP_S`` so stage i, its connector and its comet fire together.
STEP_S = 1.2

CSS = """
:root{--ink:#14202e;--muted:#54616e;--line:rgba(20,32,46,.16);--panel:#fff;--base:#f6f8fa;
--red:#e8362d;--cyan:#1c8fb8;--viz:#0ca678;--amber:#c77f0a;--violet:#7c5cff}
@media (prefers-color-scheme:dark){:root{--ink:#e6edf3;--muted:#9fb0c0;--line:rgba(230,237,243,.18);
--panel:#161b22;--base:#0d1117}}
/* timed walkthrough: a stage stays lit for ~1/6 of the cycle, then hands over */
@keyframes spot{0%{opacity:0}1.5%{opacity:.85}12%{opacity:.85}14.5%{opacity:0}100%{opacity:0}}
@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.014)}12%{transform:scale(1.014)}
14.5%{transform:scale(1)}100%{transform:scale(1)}}
@keyframes bar{0%{opacity:.4}1.5%{opacity:1}12%{opacity:1}14.5%{opacity:.4}100%{opacity:.4}}
/* marching dashes make a lit connector read as flow, not a static line */
@keyframes march{to{stroke-dashoffset:-24}}
@keyframes marchBack{to{stroke-dashoffset:24}}
/* the comet travels only inside its stage's slot, then rests invisible */
@keyframes travel{0%{transform:translateX(0);opacity:0}1.5%{opacity:1}12%{opacity:1}
14.5%{transform:translateX(var(--span));opacity:0}100%{transform:translateX(var(--span));opacity:0}}
@keyframes travelBack{0%{transform:translateX(0);opacity:0}1.5%{opacity:1}12%{opacity:1}
14.5%{transform:translateX(calc(-1*var(--span)));opacity:0}
100%{transform:translateX(calc(-1*var(--span)));opacity:0}}
.ring{fill:none;stroke-width:2;opacity:0;transform-box:fill-box;transform-origin:center}
.halo{fill:none;stroke-width:7;opacity:0}
.bar{opacity:.4}
.wash{opacity:0}
.fl{fill:none;stroke-width:2;stroke-linecap:round;stroke-dasharray:5 7;opacity:0}
.dot{opacity:0}
@media (prefers-reduced-motion: reduce){
  .ring,.halo,.dot,.bar,.wash,.fl{animation:none}
  .fl{opacity:.4}.dot{display:none}.bar{opacity:.75}.ring,.halo,.wash{opacity:0}
}
"""

ACCENT = ("#e8362d", "#1c8fb8", "#0ca678", "#c77f0a", "#7c5cff", "#e8362d")


def _svg(hide_l3: bool) -> str:
    """Horizontal six-stage round trip, sized by its own text so nothing clips.

    One control action runs left-to-right (red flow); a rule action returns
    right-to-left (cyan dashed) from the consumers stage back to control. The
    timed walkthrough (see module docstring) keeps the picture in step with the
    prose: stage i lights with the connector into it, and the return path lights
    when the consumers stage is active.
    """
    stages = [list(s) for s in STAGES]
    if hide_l3:
        stages[-1][2] = "L1 / L2"
        stages[-1][3] = "Extend behavior without rebuilding the Host"

    w, m = 1180, 40
    inner = w - 2 * m
    n = len(stages)
    gap = 22
    card_w = (inner - (n - 1) * gap) / n
    pad = 14
    top = 144
    SZ = dict(t=16, tz=15, ac=13, nt=12.5)
    LH = dict(t=19, tz=17, ac=16, nt=15)

    spec = []
    for en, zh, actor, note in stages:
        le = _wrap_words(en, SZ["t"], card_w - 2 * pad, 3)
        lz = _wrap_words(zh, SZ["tz"], card_w - 2 * pad, 2)
        la = _wrap_words(actor, SZ["ac"], card_w - 2 * pad, 2)
        ln = _wrap_words(note, SZ["nt"], card_w - 2 * pad, 4)
        ch = (34 + len(le) * LH["t"] + len(lz) * LH["tz"] + 8
              + len(la) * LH["ac"] + 8 + len(ln) * LH["nt"] + 10)
        spec.append((le, lz, la, ln, ch))

    H = max(s[4] for s in spec)
    card_t = top
    card_b = card_t + H
    y_ret = card_b + 44
    h = y_ret + 44
    cycle = STEP_S * n
    yc = card_t + 30  # connector height: badge row

    t = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h:.0f}" '
         f'viewBox="0 0 {w} {h:.0f}" '
         'font-family="IBM Plex Sans, Noto Sans CJK SC, sans-serif" role="img">',
         "<title>MiniHub end-to-end data flow / MiniHub 端到端数据流</title>",
         f"<style>{CSS}</style>",
         # defs: arrowheads, soft shadow, per-stage card wash + accent bar
         '<defs>'
         '<marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
         'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#e8362d"/></marker>'
         '<marker id="b" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
         'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#1c8fb8"/></marker>'
         '<filter id="soft" x="-20%" y="-20%" width="140%" height="140%">'
         '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" '
         'flood-opacity="0.10"/></filter>']
    for i, ac in enumerate(ACCENT[:n]):
        t.append(f'<linearGradient id="wash{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}" stop-opacity=".12"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".02"/></linearGradient>'
                 f'<linearGradient id="bar{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".55"/></linearGradient>')
    t.append("</defs>")

    # backdrop: a faint vertical wash keeps the flat panel from looking barren
    t.append(f'<rect width="{w}" height="{h:.0f}" fill="var(--base)"/>')
    t.append(f'<rect width="{w}" height="{top - 30}" fill="var(--panel)" opacity=".55"/>')
    t.append(f'<text x="{m}" y="46" fill="var(--ink)" font-size="23" font-weight="700">'
             + _esc("MiniHub 端到端数据流")
             + '<tspan style="fill:var(--muted)" font-weight="500" dx="14" font-size="15">'
             + _esc("end-to-end data flow") + "</tspan></text>")
    t.append(f'<text x="{m}" y="74" style="fill:var(--muted)" font-size="13.5">'
             + _esc("一次控制走完整条回路：控制 → 设备 → 点位更新 → 事件总线 → 屏幕/规则 → 商店 模块")
             + "</text>")

    # connectors first (cards draw on top of line ends)
    for i in range(n - 1):
        x0 = m + i * (card_w + gap) + card_w
        x1 = m + (i + 1) * (card_w + gap)
        ac = ACCENT[i % len(ACCENT)]
        d = i * STEP_S
        t.append(f'<path d="M{x0:.0f} {yc} H{x1:.0f}" stroke="var(--line)" '
                 f'stroke-width="2" fill="none"/>')
        # flow overlay: marches while this stage owns the spotlight
        t.append(f'<path class="fl" d="M{x0:.0f} {yc} H{x1:.0f}" stroke="{ac}" '
                 f'style="animation:march 1.1s linear infinite,spot {cycle:.1f}s '
                 f'linear infinite;animation-delay:0s,{d:.1f}s"/>')
        t.append(f'<circle class="dot" cx="{x0:.0f}" cy="{yc}" r="3.5" fill="{ac}" '
                 f'style="--span:{gap:.0f}px;animation:travel {cycle:.1f}s '
                 f'ease-in-out infinite;animation-delay:{d:.1f}s"/>')

    # return path: a rule action (stage RET_FROM) commands, next round starts at 0
    x_from = m + RET_FROM * (card_w + gap) + card_w / 2
    x_to = m + card_w / 2
    d = RET_FROM * STEP_S
    t.append(f'<path d="M{x_from:.0f} {y_ret} H{x_to:.0f}" stroke="var(--cyan)" '
             f'stroke-width="2" stroke-dasharray="5 4" fill="none" opacity=".45" '
             f'marker-end="url(#b)"/>')
    t.append(f'<path class="fl" d="M{x_from:.0f} {y_ret} H{x_to:.0f}" stroke="var(--cyan)" '
             f'style="animation:marchBack 1.1s linear infinite,spot {cycle:.1f}s '
             f'linear infinite;animation-delay:0s,{d:.1f}s"/>')
    t.append(f'<circle class="dot" cx="{x_from:.0f}" cy="{y_ret}" r="3.5" fill="var(--cyan)" '
             f'style="--span:{x_from - x_to:.0f}px;animation:travelBack {cycle:.1f}s '
             f'ease-in-out infinite;animation-delay:{d:.1f}s"/>')

    # cards
    for i, (en, zh, actor, note) in enumerate(stages):
        le, lz, la, ln, _ = spec[i]
        x = m + i * (card_w + gap)
        ac = ACCENT[i % len(ACCENT)]
        d = i * STEP_S
        t.append(f'<g filter="url(#soft)"><rect x="{x:.0f}" y="{card_t}" '
                 f'width="{card_w:.0f}" height="{H:.0f}" rx="10" fill="var(--panel)" '
                 f'stroke="var(--line)"/>')
        # stage wash: brightens only while this stage owns the spotlight
        t.append(f'<rect class="wash" x="{x:.0f}" y="{card_t}" width="{card_w:.0f}" '
                 f'height="{H:.0f}" rx="10" fill="url(#wash{i})" '
                 f'style="animation:spot {cycle:.1f}s linear infinite;'
                 f'animation-delay:{d:.1f}s"/></g>')
        # accent bar with gradient; pulses with the spotlight
        t.append(f'<rect class="bar" x="{x:.0f}" y="{card_t + 14:.0f}" width="3" '
                 f'height="{H - 28:.0f}" rx="1.5" fill="url(#bar{i})" '
                 f'style="animation:bar {cycle:.1f}s linear infinite;'
                 f'animation-delay:{d:.1f}s"/>')
        # spotlight ring + soft halo around the active card
        t.append(f'<rect class="halo" x="{x - 5:.0f}" y="{card_t - 5:.0f}" '
                 f'width="{card_w + 10:.0f}" height="{H + 10:.0f}" rx="16" stroke="{ac}" '
                 f'style="animation:spot {cycle:.1f}s linear infinite;'
                 f'animation-delay:{d:.1f}s"/>')
        t.append(f'<rect class="ring" x="{x - 3:.0f}" y="{card_t - 3:.0f}" '
                 f'width="{card_w + 6:.0f}" height="{H + 6:.0f}" rx="13" stroke="{ac}" '
                 f'style="animation:spot {cycle:.1f}s linear infinite,breath {cycle:.1f}s '
                 f'ease-in-out infinite;animation-delay:{d:.1f}s,{d:.1f}s"/>')
        # number badge with a faint static ring
        ly = card_t + 22
        for seg in le:
            t.append(f'<text x="{x + pad:.0f}" y="{ly:.0f}" fill="var(--ink)" '
                     f'font-size="16" font-weight="600">{_esc(seg)}</text>')
            ly += LH["t"]
        for seg in lz:
            t.append(f'<text x="{x + pad:.0f}" y="{ly:.0f}" fill="var(--muted)" '
                     f'font-size="15">{_esc(seg)}</text>')
            ly += LH["tz"]
        ly += 6
        for seg in la:
            t.append(f'<text x="{x + pad:.0f}" y="{ly:.0f}" fill="var(--amber)" '
                     f'font-size="13" font-weight="600">{_esc(seg)}</text>')
            ly += LH["ac"]
        ly += 4
        for seg in ln:
            t.append(f'<text x="{x + pad:.0f}" y="{ly:.0f}" fill="var(--muted)" '
                     f'font-size="12.5">{_esc(seg)}</text>')
            ly += LH["nt"]

    t.append(f'<text x="{m}" y="{y_ret + 28:.0f}" style="fill:var(--muted)" '
             f'font-size="13.5">'
             + _esc("箭头向右 = 一次控制的前进方向；青虚线向左 = 规则动作开启下一轮控制")
             + "</text>")
    t.append("</svg>")
    return "\n".join(t)


def _html(hide_l3: bool) -> str:
    stages = [list(s) for s in STAGES]
    if hide_l3:
        stages[-1][2] = "L1 / L2"
        stages[-1][3] = "Extend behavior without rebuilding the Host"
    cards = "".join(
        '<article class="step" data-i="%d">'
        '<h3>%s <span>%s</span></h3><p class="actor">%s</p><p class="note">%s</p></article>'
        % (i, _esc(en), _esc(zh), _esc(actor), _esc(note))
        for i, (en, zh, actor, note) in enumerate(stages))
    step_ms = int(STEP_S * 1000)
    # Placeholders, not %-format: the HTML template carries its own CSS with
    # percent signs (``height:100%``), which %-formatting would choke on.
    html = ("""<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MiniHub 端到端数据流 · end-to-end data flow</title><style>""" + CSS + """
body{margin:0;padding:28px;background:var(--base);color:var(--ink);font:16.5px/1.65 IBM Plex Sans,"Noto Sans CJK SC",sans-serif}
h1{font-size:25px;margin:0 0 6px}p.lead{color:var(--muted);margin:0 0 14px;font-size:14.5px;max-width:80ch}
/* tour progress: one bar, one cycle, no layout shift */
.tour{height:3px;border-radius:2px;background:var(--line);overflow:hidden;margin:0 0 22px}
.tour>i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--red),var(--amber),var(--cyan));
animation:fill linear infinite}
@keyframes fill{from{width:0}to{width:100%}}
.steps{display:flex;flex-wrap:wrap;gap:12px}
.step{border:1px solid var(--line);border-radius:12px;background:var(--panel);padding:14px 16px;margin:0;flex:1 1 280px;
transition:box-shadow .25s,transform .25s,border-color .25s}
.step.on{border-color:var(--red);box-shadow:0 0 0 2px var(--red) inset,0 6px 18px rgba(20,32,46,.08);transform:translateY(-3px)}
.step h3{margin:0 0 8px;font-size:16px}
.step h3 span{color:var(--muted);font-weight:400;font-size:15px;margin-left:8px}
.actor{color:var(--amber);font-size:13.5px;font-weight:600;margin:0 0 8px}
.note{color:var(--muted);font-size:13.5px;margin:0}
@media (max-width:720px){.step{flex:1 1 100%}}
</style></head><body>
<h1>MiniHub 端到端数据流 · end-to-end data flow</h1>
<p class="lead">一次控制走完整条回路：控制 → 设备 → 点位更新 → 事件总线 → 屏幕/规则 → 商店 模块。
卡片自动逐站导览，悬停暂停；每张卡承担一项约束。<br>One control round trip in six stages. The tour advances on its own and pauses on hover.</p>
<div class="tour"><i id="bar" style="animation-duration:__CYCLE__ms"></i></div>
<main class="steps" id="steps">__CARDS__</main>
<p id="say" style="margin-top:18px;color:var(--muted);font-size:14px">本页不请求外部资源，离线可读。</p>
<script>
(function(){
  var steps=[].slice.call(document.querySelectorAll('.step'));
  var say=document.getElementById('say'), bar=document.getElementById('bar');
  var i=0, timer=null;
  var still=window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  function show(k){
    steps.forEach(function(s,j){s.classList.toggle('on',j===k)});
    var st=steps[k]; say.textContent=(k+1)+'. '+st.querySelector('h3').textContent+' — '+st.querySelector('.actor').textContent;
  }
  function play(){ if(still) return; timer=setInterval(function(){i=(i+1)%steps.length;show(i)}, __STEP__); }
  function pause(){ if(timer){clearInterval(timer);timer=null;} }
  show(0);
  if(!still){
    play();
    document.getElementById('steps').addEventListener('mouseenter',pause);
    document.getElementById('steps').addEventListener('mouseleave',play);
    document.getElementById('steps').addEventListener('focusin',pause);
  }
})();
</script></body></html>
""")
    return (html.replace("__CARDS__", cards)
                .replace("__STEP__", str(step_ms))
                .replace("__CYCLE__", str(step_ms * len(stages))))


def stage_data_flow_diagram(hide_l3: bool = False) -> Path:
    """Write the figure pair for this documentation variant.

    The file names do not follow the flag: a build that publishes no native tier
    should not name one in its asset URLs either — and a leftover copy of the
    other variant rides into the published tree through html_static_path, where
    its native band stays reachable.
    """
    _STAGED.mkdir(parents=True, exist_ok=True)
    svg = _STAGED / "data_flow.svg"
    html = _STAGED / "index.html"
    write_text(svg, _svg(hide_l3))
    write_text(html, _html(hide_l3))
    # Sphinx keys _images/ by basename, so a shared flow.svg became flow1.svg.
    for stale in (_STAGED / "flow.svg", _STAGED / "flow_no_l3.svg",
                  _STAGED / "index_no_l3.html"):
        stale.unlink(missing_ok=True)
    return _STAGED

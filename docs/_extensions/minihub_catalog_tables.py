# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Render the shipped type catalog and stage the data-model figures.

``.. minihub-catalog:: devices`` and ``.. minihub-catalog:: parameters`` read
``components/minihub_device_widget/model/*.csv`` at build time, so the guide
cannot advertise a type, parameter or widget the firmware does not ship. The
figures are drawn here too; do not edit the staged copies by hand.

Widget names come from the same mapping the Host uses
(``dw_ui_to_widget``), and the default access comes from the catalog
``properties`` column (``Read`` -> R, anything with ``Write`` -> RW).
"""

from __future__ import annotations

import csv
from pathlib import Path
from xml.sax.saxutils import escape as _esc

from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.statemachine import StringList
from minihub_figure_kit import write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_REPO = _DOCS_DIR.parent
_WIDGET = _REPO / "components" / "minihub_device_widget"
_MODEL = _WIDGET / "model"
_ICONS = _WIDGET / "images" / "devices"
_STAGED = _DOCS_DIR / "_static" / "data_model"

_ICON_REL = "../_static/catalog/images/devices"


def _rows(name: str) -> list[dict]:
    with (_MODEL / name).open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def _is_zh(lang) -> bool:
    return str(lang or "").lower().startswith("zh")


def _cell(row: dict, key: str) -> str:
    return (row.get(key) or "").strip()


def _params_of(row: dict) -> list[str]:
    return [p.strip() for p in _cell(row, "parameters").split(",") if p.strip()]


def _table(header: list[str], widths: list[int], rows: list[list[str]]) -> list[str]:
    out = [
        ".. list-table::",
        "   :header-rows: 1",
        "   :class: mh-catalog",
        "   :widths: " + " ".join(str(w) for w in widths),
        "",
        "   * - " + header[0],
    ]
    out += ["     - " + h for h in header[1:]]
    for row in rows:
        for i, cell in enumerate(row):
            lead = "   * - " if i == 0 else "     - "
            if isinstance(cell, list):
                out.append(lead + "| " + cell[0])
                out += ["       | " + ln for ln in cell[1:]]
                continue
            lines = str(cell).splitlines() or [""]
            out.append(lead + lines[0])
            out += ["       " + ln for ln in lines[1:]]
    out.append("")
    return out


def _chunks(items: list[str], limit: int) -> list[str]:
    lines: list[str] = []
    cur = ""
    for item in items:
        cand = f"{cur}, {item}" if cur else item
        if cur and len(cand) > limit:
            lines.append(cur + ",")
            cur = item
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return lines


def devices_rst(lang) -> list[str]:
    zh = _is_zh(lang)
    header = (["图标", "类型", "参数（类型全集）", "主参数", "必选"] if zh
              else ["Icon", "Type", "Parameters (whole type)", "Primary", "Required"])
    rows = []
    for row in _rows("devices.csv"):
        did = _cell(row, "device_id")
        if not did:
            continue
        if (_ICONS / f"{did}.svg").is_file():
            icon = f".. image:: {_ICON_REL}/{did}.svg\n   :width: 56px\n   :alt: {did}"
        else:
            icon = "—"
        rows.append([
            icon,
            f":chip:`{did}`",
            _chunks(_params_of(row), 26),
            f"``{_cell(row, 'primary_param')}``" if _cell(row, "primary_param") else "—",
            ", ".join(f"``{p}``" for p in
                      (x.strip() for x in _cell(row, "mandatory_params").split(","))
                      if p),
        ])
    return _table(header, [8, 16, 30, 14, 18], rows)


def parameters_rst(lang) -> list[str]:
    zh = _is_zh(lang)
    used: dict[str, list[str]] = {}
    for dev in _rows("devices.csv"):
        for name in _params_of(dev):
            used.setdefault(name, []).append(_cell(dev, "device_id"))

    header = (["参数", "标签", "类型", "默认范围", "控件", "用在"] if zh
              else ["Parameter", "Label", "Type", "Default range", "Widget", "Used by"])
    rows = []
    for row in _rows("parameters.csv"):
        name = _cell(row, "parameter")
        if not name:
            continue
        label = _cell(row, "label_zh" if zh else "label_en") or \
            _cell(row, "label_zh") or name
        dtype = _cell(row, "data_type") or "—"

        lo, hi = _cell(row, "min"), _cell(row, "max")
        step, unit = _cell(row, "step"), _cell(row, "unit")
        if lo and hi:
            rng = f"{lo}–{hi}" + (f" {unit}" if unit else "")
            if step:
                rng += f"（步长 {step}）" if zh else f", step {step}"
        else:
            rng = unit or "—"

        ui = _cell(row, "ui_element")
        widget = ui[3:] if ui.startswith("ui.") else (ui or "text")
        suffix = ""
        props = _cell(row, "properties").lower()
        if "write" not in props and "read" in props:
            suffix = " （只读）" if zh else " (read-only)"

        rows.append([
            f"``{name}``",
            label,
            f":chip:`{dtype}`" if dtype != "—" else "—",
            rng,
            f":chip:`{widget}`{suffix}",
            _chunks(used.get(name, []), 30) or "—",
        ])
    return _table(header, [14, 12, 8, 20, 17, 29], rows)


# --- figures ---------------------------------------------------------------

_PALETTE = """
:root{--ink:#14202e;--muted:#54616e;--line:rgba(20,32,46,.16);--panel:#fff;--base:#f6f8fa;
--red:#e8362d;--cyan:#1c8fb8;--viz:#0ca678;--amber:#c77f0a;--accent:#6366f1}
@media (prefers-color-scheme:dark){:root{--ink:#e6edf3;--muted:#9fb0c0;--line:rgba(230,237,243,.18);
--panel:#161b22;--base:#0d1117}}
"""

_FONT = "IBM Plex Sans, Noto Sans CJK SC, sans-serif"
_MONO = "IBM Plex Mono, Menlo, monospace"


_LIFECYCLE_TEXT = {
    "zh": {
        "title": "点位模型",
        "sub": "一台设备一条点位 · 状态向上到用户，命令沿右侧回到设备",
        "device_t": "物理设备",
        "device_note": "一台设备只登记一条点位",
        "device_ex": "Zigbee 灯 · Matter endpoint · MQTT 插座 · L2 插件点",
        "a1": "① 状态上报：适配器写 props.params",
        "point_t": "点位",
        "ui_tag": "UI = 目录 ∩ props.params",
        "note1": "目录给形状与范围，点位给数值；",
        "note2": "不在 params 里的参数不画行。",
        "a2": "② 概览页 · 规则 · 场景读同一份",
        "cons_t": "用户",
        "items": [
            ("概览页", "GET /api/device_widgets ∩ props.params"),
            ("规则", "when.parameter → props.params.<name>.value"),
            ("场景 · REST", "actions[].params → 适配器命令"),
        ],
        "lane": ["③ 命令", "经适配器", "到设备"],
        "legend": "红点向上 = 状态上报 · 琥珀虚线向下 = 命令下发，生效后回到 ①",
        "aria": "MiniHub 点位模型：适配器把设备状态写进 props.params，用户通过概览页、规则与场景读同一份，命令经适配器回到设备",
    },
    "en": {
        "title": "Point model",
        "sub": "one device, one point — state rises to the user, commands return on the right",
        "device_t": "Physical device",
        "device_note": "one physical device registers one point",
        "device_ex": "Zigbee bulb · Matter endpoint · MQTT plug · L2 plugin",
        "a1": "① state up: adapter writes props.params",
        "point_t": "Point",
        "ui_tag": "UI = catalog ∩ props.params",
        "note1": "the catalog supplies shape and range, the point supplies values;",
        "note2": "a parameter absent from props.params has no row.",
        "a2": "② devices, rules, scenes read the same data",
        "cons_t": "User",
        "items": [
            ("Overview page", "GET /api/device_widgets ∩ props.params"),
            ("Rules", "when.parameter → props.params.<name>.value"),
            ("Scenes · REST", "actions[].params → adapter command"),
        ],
        "lane": ["③ command", "via adapter", "to device"],
        "legend": "red moves up = state reported · amber dashed down = command, then back to ①",
        "aria": "MiniHub point model: the adapter writes the device state into props.params, the user reads it through the Overview page, rules and scenes, and commands go through the adapter back to the device",
    },
}


# Spotlight accents for the three lifecycle cards: user, point, device.
_LIFE_ACCENT = ("#1c8fb8", "#6366f1", "#e8362d")
_LIFE_STEP_S = 1.3


def _lifecycle_svg(lang) -> str:
    """Top-down: user -> point -> device; state rises, commands return right.

    A timed walkthrough in reading order (user, point, device): each card takes
    the spotlight in turn, and the arrow that belongs to that stage flows while
    it is lit — the command lane during the user stage, the rising state arrows
    during the point and device stages. All motion is CSS, so it survives
    ``<img>`` embedding and stops under ``prefers-reduced-motion``.
    """
    T = _LIFECYCLE_TEXT["zh" if _is_zh(lang) else "en"]
    dy = 8  # gap between the header panel and the first card
    w, h = 780, 812 + dy
    cx = 330
    card_x, card_w = 40, 580
    user = (card_x, 92 + dy, card_w, 166)
    pt = (card_x, 300 + dy, card_w, 316)
    dev = (card_x, 660 + dy, card_w, 104)
    lane_x = 670
    cards = (user, pt, dev)
    cycle = _LIFE_STEP_S * len(cards)

    t = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" '
        f'font-family="{_FONT}" role="img">',
        f"<title>{_esc(T['aria'])}</title>",
        f"<style>{_PALETTE}"
        ".card{fill:var(--panel);stroke:var(--line)}"
        ".t{fill:var(--ink);font-size:16px;font-weight:700}"
        ".s{fill:var(--muted);font-size:13px}"
        ".m{fill:var(--ink);font-size:12.5px;font-family:" + _MONO + "}"
        ".k{fill:var(--muted);font-size:12px}"
        ".tag{fill:var(--amber);font-size:12.5px;font-weight:600}"
        ".cap{fill:var(--muted);font-size:13px}"
        ".up{stroke:var(--red);stroke-width:2;fill:none;marker-end:url(#ma)}"
        ".dn{stroke:var(--amber);stroke-width:2;fill:none;stroke-dasharray:6 4;"
        "marker-end:url(#mb)}"
        # timed walkthrough: one card lit at a time, its arrow flowing with it
        "@keyframes spot{0%{opacity:0}1.5%{opacity:.85}12%{opacity:.85}"
        "14.5%{opacity:0}100%{opacity:0}}"
        "@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.008)}"
        "12%{transform:scale(1.008)}14.5%{transform:scale(1)}100%{transform:scale(1)}}"
        "@keyframes bar{0%{opacity:.4}1.5%{opacity:1}12%{opacity:1}"
        "14.5%{opacity:.4}100%{opacity:.4}}"
        "@keyframes marchV{to{stroke-dashoffset:-24}}"
        "@keyframes travelUp{0%{transform:translateY(0);opacity:0}1.5%{opacity:1}"
        "12%{opacity:1}14.5%{transform:translateY(-26px);opacity:0}"
        "100%{transform:translateY(-26px);opacity:0}}"
        "@keyframes travelDown{0%{transform:translateY(0);opacity:0}1.5%{opacity:1}"
        "12%{opacity:1}14.5%{transform:translateY(490px);opacity:0}"
        "100%{transform:translateY(490px);opacity:0}}"
        ".ring{fill:none;stroke-width:2;opacity:0;transform-box:fill-box;"
        "transform-origin:center}"
        ".halo{fill:none;stroke-width:7;opacity:0}"
        ".bar{opacity:.4}"
        ".wash{opacity:0}"
        ".fl{fill:none;stroke-width:2;stroke-linecap:round;stroke-dasharray:5 7;"
        "opacity:0}"
        ".dot{opacity:0}"
        "@media (prefers-reduced-motion: reduce){.ring,.halo,.bar,.wash,.fl,.dot"
        "{animation:none}.fl{opacity:.45}.dot{display:none}.bar{opacity:.75}"
        ".ring,.halo,.wash{opacity:0}}"
        "</style>",
        '<defs>'
        '<marker id="ma" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
        'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#e8362d"/>'
        '</marker>'
        '<marker id="mb" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
        'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#c77f0a"/>'
        '</marker>'
        '<filter id="soft" x="-20%" y="-20%" width="140%" height="140%">'
        '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" '
        'flood-opacity="0.10"/></filter>',
    ]
    for i, ac in enumerate(_LIFE_ACCENT):
        t.append(f'<linearGradient id="lwash{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}" stop-opacity=".12"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".02"/>'
                 f'</linearGradient>'
                 f'<linearGradient id="lbar{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".55"/>'
                 f'</linearGradient>')
    t.append("</defs>")

    t.append(f'<rect width="{w}" height="{h}" fill="var(--base)"/>')
    t.append(f'<rect width="{w}" height="84" fill="var(--panel)" opacity=".55"/>')
    t.append(f'<text x="40" y="44" fill="var(--ink)" font-size="23" '
             f'font-weight="700">{_esc(T["title"])}</text>')
    t.append(f'<text x="40" y="70" class="cap">{_esc(T["sub"])}</text>')

    # lanes first, so the cards sit on top of the line ends
    t += [
        f'<path class="up" d="M{cx} {292 + dy} V{266 + dy}"/>',
        f'<path class="up" d="M{cx} {652 + dy} V{624 + dy}"/>',
        f'<path class="dn" d="M620 {180 + dy} H{lane_x} V{712 + dy} H632"/>',
        f'<text x="346" y="{284 + dy}" class="tag">{_esc(T["a2"])}</text>',
        f'<text x="346" y="{646 + dy}" class="tag">{_esc(T["a1"])}</text>',
    ]
    for i, line in enumerate(T["lane"]):
        t.append(f'<text x="678" y="{430 + dy + i * 18}" class="k">{_esc(line)}</text>')
    # command lane flows while the user card owns the spotlight (stage 0)
    t.append(f'<path class="fl" d="M{lane_x} {200 + dy} V{700 + dy}" '
             f'stroke="var(--amber)" style="animation:marchV 1.1s linear infinite,'
             f'spot {cycle:.2f}s linear infinite;animation-delay:0s,0.00s"/>')
    t.append(f'<circle class="dot" cx="{lane_x}" cy="{200 + dy}" r="4" '
             f'fill="var(--amber)" style="--span:490px;animation:travelDown '
             f'{cycle:.2f}s ease-in-out infinite;animation-delay:0.00s"/>')
    # state rises: the point stage (1) lights the upper arrow, the device
    # stage (2) the lower one
    for idx, (y_from, y_to) in enumerate(((292, 266), (652, 624))):
        d = (idx + 1) * _LIFE_STEP_S
        t.append(f'<path class="fl" d="M{cx} {y_from + dy} V{y_to + dy}" '
                 f'stroke="var(--red)" style="animation:marchV 1.1s linear '
                 f'infinite,spot {cycle:.2f}s linear infinite;'
                 f'animation-delay:0s,{d:.2f}s"/>')
        t.append(f'<circle class="dot" cx="{cx}" cy="{y_from + dy}" r="4" '
                 f'fill="var(--red)" style="--span:26px;animation:travelUp '
                 f'{cycle:.2f}s ease-in-out infinite;animation-delay:{d:.2f}s"/>')

    # the three cards, each with its own spotlight slot
    for i, (x, y, cw, ch) in enumerate(cards):
        ac = _LIFE_ACCENT[i]
        d = i * _LIFE_STEP_S
        t.append(f'<g filter="url(#soft)"><rect x="{x}" y="{y}" width="{cw}" '
                 f'height="{ch}" rx="12" fill="var(--panel)" stroke="var(--line)"/>')
        t.append(f'<rect class="wash" x="{x}" y="{y}" width="{cw}" height="{ch}" '
                 f'rx="12" fill="url(#lwash{i})" style="animation:spot {cycle:.2f}s '
                 f'linear infinite;animation-delay:{d:.2f}s"/></g>')
        t.append(f'<rect class="bar" x="{x}" y="{y + 14}" width="3" '
                 f'height="{ch - 28}" rx="1.5" fill="url(#lbar{i})" '
                 f'style="animation:bar {cycle:.2f}s linear infinite;'
                 f'animation-delay:{d:.2f}s"/>')
        t.append(f'<rect class="halo" x="{x - 5}" y="{y - 5}" width="{cw + 10}" '
                 f'height="{ch + 10}" rx="17" stroke="{ac}" style="animation:spot '
                 f'{cycle:.2f}s linear infinite;animation-delay:{d:.2f}s"/>')
        t.append(f'<rect class="ring" x="{x - 3}" y="{y - 3}" width="{cw + 6}" '
                 f'height="{ch + 6}" rx="15" stroke="{ac}" style="animation:spot '
                 f'{cycle:.2f}s linear infinite,breath {cycle:.2f}s ease-in-out '
                 f'infinite;animation-delay:{d:.2f}s,{d:.2f}s"/>')

    # the user
    x, y, cw, ch = user
    t.append(f'<text x="{x + 18}" y="{y + 28}" class="t">{_esc(T["cons_t"])}</text>')
    for i, (name, detail) in enumerate(T["items"]):
        yy = y + 58 + i * 36
        t += [
            f'<text x="{x + 18}" y="{yy}" class="m">{_esc(name)}</text>',
            f'<text x="{x + 18}" y="{yy + 16}" class="k">{_esc(detail)}</text>',
        ]
        if i < len(T["items"]) - 1:
            t.append(f'<line x1="{x + 18}" y1="{yy + 24}" x2="{x + cw - 18}" '
                     f'y2="{yy + 24}" stroke="var(--line)"/>')

    # the point
    x, y, cw, ch = pt
    t += [
        f'<text x="{x + 18}" y="{y + 30}" class="t">{_esc(T["point_t"])}</text>',
        f'<text x="{x + 18}" y="{y + 56}" class="m">minihub_point_t</text>',
        f'<text x="{x + 18}" y="{y + 76}" class="m">point_id = sim.js_kitchen</text>',
        f'<text x="{x + 18}" y="{y + 96}" class="m">type = light   value = on</text>',
        f'<text x="{x + 18}" y="{y + 122}" class="tag">{_esc(T["ui_tag"])}</text>',
    ]
    rows = [("power", "true", "RW"), ("brightness", "80 %", "RW"),
            ("hue", "120 °", "RW"), ("saturation", "60 %", "RW"),
            ("cct", "4000 K", "visible:false")]
    for i, (name, val, acc) in enumerate(rows):
        yy = y + 146 + i * 20
        t += [
            f'<text x="{x + 18}" y="{yy}" class="m">{_esc(name)}</text>',
            f'<text x="{x + 200}" y="{yy}" class="m">{_esc(val)}</text>',
            f'<text x="{x + 330}" y="{yy}" class="k">{_esc(acc)}</text>',
        ]
    t += [
        f'<line x1="{x + 18}" y1="{y + 246}" x2="{x + cw - 18}" y2="{y + 246}" '
        'stroke="var(--line)"/>',
        f'<text x="{x + 18}" y="{y + 270}" class="tag">{_esc(T["ui_tag"])}</text>',
        f'<text x="{x + 18}" y="{y + 290}" class="k">{_esc(T["note1"])}</text>',
        f'<text x="{x + 18}" y="{y + 308}" class="k">{_esc(T["note2"])}</text>',
    ]

    # physical device
    x, y, cw, ch = dev
    t += [
        f'<text x="{x + 18}" y="{y + 30}" class="t">{_esc(T["device_t"])}</text>',
        f'<text x="{x + 18}" y="{y + 56}" class="k">{_esc(T["device_note"])}</text>',
        f'<text x="{x + 18}" y="{y + 84}" class="m">{_esc(T["device_ex"])}</text>',
    ]

    t += [
        f'<text x="40" y="{796 + dy}" class="cap">{_esc(T["legend"])}</text>',
        "</svg>",
    ]
    return "\n".join(t)


# One row performs per second; the full tour is one second per widget.
_W_STEP_S = 1.0
_W_ACCENT = ("#e8362d", "#1c8fb8", "#0ca678", "#c77f0a", "#7c5cff", "#e8362d")


def _widgets_svg() -> str:
    """The parameter row styles: what each ``ui_element`` renders as.

    A timed walkthrough down the six rows: while a row owns the spotlight its
    mockup performs — the toggle flips, the slider sweeps, the dropdown opens,
    the reading ticks, the button presses, and the hidden row fades in and out.
    Every animation is slot-relative (the action sits in the first half of the
    row's 1 s slot) and offset by ``row * 1 s``, so the figure demos the widgets
    in the order the prose lists them. Base states are the resting ones, so a
    ``prefers-reduced-motion`` build shows a correct still picture.
    """
    rows = [
        ("ui.toggle", "开关", "power"),
        ("ui.slider", "滑杆", "brightness / hue / cct"),
        ("ui.dropdown", "下拉", "options 列"),
        ("ui.text", "文本", "name / 只读读数"),
        ("ui.button", "按钮", "play_pause / mute"),
        ("ui.hidden", "隐藏", "类型级不渲染"),
    ]
    # per-row card heights: only the dropdown needs room for its open menu
    heights = (56, 60, 108, 56, 56, 56)
    top = 104
    # the footer note rides below the last row, and the canvas below the note,
    # so neither is a magic number that has to be re-derived when a row moves
    rows_bottom = top + sum(heights) + 8 * (len(heights) - 1)
    foot_y = rows_bottom + 30
    w, h = 1000, foot_y + 18
    cycle = _W_STEP_S * len(rows)
    slot = 100.0 / len(rows)  # one row's share of the cycle

    def _kf(name, stops):
        """Emit a keyframe set whose stops are authored against one row's slot.

        The stops read 0–100 % of the row's 1 s slot; the cycle is
        ``len(rows)`` seconds, so every stop is scaled to its share of the
        cycle. A stop at 100 means "rest of the cycle".
        """
        body = "".join(
            (f"{p * slot / 100:.2f}%{{{d}}}" if p != 100 else f"100%{{{d}}}")
            for p, d in stops)
        return f"@keyframes {name}{{{body}}}"

    # every mockup performs inside its own slot: authored slot-relative, then
    # scaled to the shared cycle by _kf
    kf = [
        _kf("knob", [(0, "transform:translateX(0)"), (15, "transform:translateX(0)"),
                     (30, "transform:translateX(20px)"), (45, "transform:translateX(20px)"),
                     (60, "transform:translateX(0)"), (100, "transform:translateX(0)")]),
        _kf("fillX", [(0, "transform:scaleX(0)"), (15, "transform:scaleX(0)"),
                      (30, "transform:scaleX(1)"), (45, "transform:scaleX(1)"),
                      (60, "transform:scaleX(0)"), (100, "transform:scaleX(0)")]),
        _kf("sweep", [(0, "transform:translateX(38px)"), (15, "transform:translateX(38px)"),
                      (30, "transform:translateX(200px)"), (45, "transform:translateX(342px)"),
                      (60, "transform:translateX(38px)"), (100, "transform:translateX(38px)")]),
        _kf("sldfill", [(0, "transform:scaleX(.1)"), (15, "transform:scaleX(.1)"),
                        (30, "transform:scaleX(.52)"), (45, "transform:scaleX(.9)"),
                        (60, "transform:scaleX(.1)"), (100, "transform:scaleX(.1)")]),
        _kf("vA", [(0, "opacity:1"), (30, "opacity:1"), (33, "opacity:0"),
                   (60, "opacity:0"), (63, "opacity:1"), (100, "opacity:1")]),
        _kf("vB", [(0, "opacity:0"), (30, "opacity:0"), (33, "opacity:1"),
                   (45, "opacity:1"), (48, "opacity:0"), (100, "opacity:0")]),
        _kf("vC", [(0, "opacity:0"), (45, "opacity:0"), (48, "opacity:1"),
                   (60, "opacity:1"), (63, "opacity:0"), (100, "opacity:0")]),
        _kf("dd", [(0, "opacity:0;transform:translateY(-6px)"),
                   (15, "opacity:1;transform:translateY(0)"),
                   (45, "opacity:1;transform:translateY(0)"),
                   (60, "opacity:0;transform:translateY(-6px)"),
                   (100, "opacity:0;transform:translateY(-6px)")]),
        _kf("hl", [(0, "transform:translateY(0)"), (20, "transform:translateY(0)"),
                   (40, "transform:translateY(28px)"), (55, "transform:translateY(28px)"),
                   (70, "transform:translateY(0)"), (100, "transform:translateY(0)")]),
        _kf("rA", [(0, "opacity:1"), (20, "opacity:1"), (25, "opacity:0"),
                   (70, "opacity:0"), (75, "opacity:1"), (100, "opacity:1")]),
        _kf("rB", [(0, "opacity:0"), (20, "opacity:0"), (25, "opacity:1"),
                   (45, "opacity:1"), (50, "opacity:0"), (100, "opacity:0")]),
        _kf("rC", [(0, "opacity:0"), (45, "opacity:0"), (50, "opacity:1"),
                   (70, "opacity:1"), (75, "opacity:0"), (100, "opacity:0")]),
        _kf("pressA", [(0, "transform:scale(1)"), (15, "transform:scale(1)"),
                       (25, "transform:scale(.92)"), (35, "transform:scale(1)"),
                       (100, "transform:scale(1)")]),
        _kf("pressB", [(0, "transform:scale(1)"), (35, "transform:scale(1)"),
                       (45, "transform:scale(.92)"), (55, "transform:scale(1)"),
                       (100, "transform:scale(1)")]),
        _kf("ghost", [(0, "opacity:.3"), (30, "opacity:.65"), (60, "opacity:.3"),
                      (100, "opacity:.3")]),
    ]

    ys, y = [], top
    for hh in heights:
        ys.append(y)
        y += hh + 8

    t = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" '
        f'font-family="{_FONT}" role="img">',
        "<title>MiniHub parameter widgets / MiniHub 参数控件样式</title>",
        f"<style>{_PALETTE}"
        ".t{fill:var(--ink);font-size:16px;font-weight:700}"
        ".s{fill:var(--muted);font-size:13px}"
        ".z{fill:var(--muted);font-size:12.5px}"
        ".cap{fill:var(--muted);font-size:13px}"
        ".card{fill:var(--panel);stroke:var(--line)}"
        ".mono{fill:var(--ink);font-size:12.5px;font-family:" + _MONO + "}"
        ".box{fill:var(--panel);stroke:var(--line)}"
        ".btn{fill:var(--panel);stroke:var(--line)}"
        ".knob{fill:#fff}"
        ".track{fill:var(--line)}"
        ".fill{fill:var(--accent)}"
        ".thumb{fill:#fff;stroke:var(--accent);stroke-width:2}"
        ".menu{fill:var(--panel);stroke:var(--line)}"
        ".hilite{fill:var(--accent);opacity:.14}"
        # timed walkthrough: one row lit at a time
        "@keyframes spot{0%{opacity:0}1.5%{opacity:.85}12%{opacity:.85}"
        "14.5%{opacity:0}100%{opacity:0}}"
        "@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.006)}"
        "12%{transform:scale(1.006)}14.5%{transform:scale(1)}100%{transform:scale(1)}}"
        ".ring{fill:none;stroke-width:2;opacity:0;transform-box:fill-box;"
        "transform-origin:center}"
        ".halo{fill:none;stroke-width:7;opacity:0}"
        ".wash{opacity:0}"
        # base states: the resting picture a reduced-motion build shows
        ".tgl-knob{transform:translateX(0)}"
        ".tgl-fill{transform:scaleX(0);transform-box:fill-box;"
        "transform-origin:left center}"
        ".sld-thumb{transform:translateX(38px)}"
        ".sld-fill{transform:scaleX(.1);transform-box:fill-box;"
        "transform-origin:left center}"
        ".dd-menu{opacity:0}"
        ".ghost{opacity:.3}"
        # every mockup performs inside its own slot; the stops above are
        # authored slot-relative and scaled to the shared cycle
        + " ".join(kf) +
        "@media (prefers-reduced-motion: reduce){.ring,.halo,.wash,.tgl-knob,"
        ".tgl-fill,.sld-thumb,.sld-fill,.dd-menu,.hl,.press,.ghost{animation:none}"
        ".ring,.halo,.wash{opacity:0}}"
        "</style>",
        '<defs><filter id="soft" x="-20%" y="-20%" width="140%" height="140%">'
        '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" '
        'flood-opacity="0.10"/></filter></defs>',
        f'<rect width="{w}" height="{h}" fill="var(--base)"/>',
        f'<rect width="{w}" height="86" fill="var(--panel)" opacity=".55"/>',
        '<text x="40" y="46" fill="var(--ink)" font-size="23" font-weight="700">'
        '参数控件样式'
        '<tspan class="s" font-weight="500" dx="14">widget styles</tspan></text>',
        '<text x="40" y="76" class="cap">'
        '参数行由 device_widgets.css 按目录的 ui_element 画；参数行没有图标。</text>',
    ]

    for i, ((name, zh, ex), y0, hh) in enumerate(zip(rows, ys, heights)):
        ac = _W_ACCENT[i % len(_W_ACCENT)]
        d = i * _W_STEP_S
        my = y0 + 26  # mockup centre line
        key = name[3:]
        cx0, cw = 330, 630
        # row card with its own spotlight slot
        t.append(f'<g filter="url(#soft)"><rect x="{cx0}" y="{y0 + 2}" '
                 f'width="{cw}" height="{hh - 4}" rx="10" fill="var(--panel)" '
                 f'stroke="var(--line)"/>')
        t.append(f'<rect class="wash" x="{cx0}" y="{y0 + 2}" width="{cw}" '
                 f'height="{hh - 4}" rx="10" fill="{ac}" fill-opacity=".06" '
                 f'style="animation:spot {cycle:.1f}s linear infinite;'
                 f'animation-delay:{d:.1f}s"/></g>')
        t.append(f'<rect class="halo" x="{cx0 - 5}" y="{y0 - 3}" width="{cw + 10}" '
                 f'height="{hh + 6}" rx="16" stroke="{ac}" style="animation:spot '
                 f'{cycle:.1f}s linear infinite;animation-delay:{d:.1f}s"/>')
        t.append(f'<rect class="ring" x="{cx0 - 3}" y="{y0 - 1}" width="{cw + 6}" '
                 f'height="{hh + 2}" rx="13" stroke="{ac}" style="animation:spot '
                 f'{cycle:.1f}s linear infinite,breath {cycle:.1f}s ease-in-out '
                 f'infinite;animation-delay:{d:.1f}s,{d:.1f}s"/>')
        # gutter label
        t.append(f'<text x="40" y="{y0 + 24}" class="mono">{name}</text>')
        t.append(f'<text x="40" y="{y0 + 42}" class="z">{zh} · {ex}</text>')

        anim = (f"animation-duration:{cycle:.1f}s;animation-delay:{d:.1f}s;"
                f"animation-iteration-count:infinite;"
                f"animation-timing-function:ease-in-out;")

        if key == "toggle":
            t += [
                f'<rect x="352" y="{my - 14}" width="48" height="28" rx="14" '
                f'class="track"/>',
                f'<rect class="tgl-fill" x="352" y="{my - 14}" width="48" '
                f'height="28" rx="14" fill="var(--accent)" style="{anim}'
                f'animation-name:fillX"/>',
                f'<circle class="tgl-knob" cx="366" cy="{my}" r="11" fill="#fff" '
                f'style="{anim}animation-name:knob"/>',
                f'<text x="412" y="{my + 5}" class="z">off</text>',
                f'<text x="512" y="{my + 5}" class="z">on</text>',
            ]
        elif key == "slider":
            t += [
                f'<rect x="352" y="{my - 3}" width="380" height="6" rx="3" '
                f'class="track"/>',
                f'<rect class="sld-fill" x="352" y="{my - 3}" width="380" '
                f'height="6" rx="3" fill="var(--accent)" style="{anim}'
                f'animation-name:sldfill"/>',
                f'<circle class="sld-thumb" cx="352" cy="{my}" r="9" fill="#fff" '
                f'stroke="var(--accent)" stroke-width="2" style="{anim}'
                f'animation-name:sweep"/>',
                f'<text class="mono" x="748" y="{my + 5}" style="{anim}'
                f'animation-name:vA">10 %</text>',
                f'<text class="mono" x="748" y="{my + 5}" style="{anim}'
                f'animation-name:vB">50 %</text>',
                f'<text class="mono" x="748" y="{my + 5}" style="{anim}'
                f'animation-name:vC">90 %</text>',
                f'<text x="352" y="{my + 24}" class="z">'
                f'min 1 · max 100 · step 1 · 依赖 power</text>',
            ]
        elif key == "dropdown":
            t += [
                f'<rect x="352" y="{my - 16}" width="220" height="32" rx="8" '
                f'class="box"/>',
                f'<text x="366" y="{my + 5}" class="mono">Auto</text>',
                f'<path d="M548 {my - 3} l8 8 l8 -8" stroke="var(--muted)" '
                f'stroke-width="2" fill="none"/>',
                f'<g class="dd-menu" style="{anim}animation-name:dd">'
                f'<rect x="352" y="{my + 16}" width="220" height="84" rx="8" '
                f'class="menu" filter="url(#soft)"/>'
                f'<rect class="hilite" x="356" y="{my + 18}" width="212" '
                f'height="24" rx="6" style="{anim}animation-name:hl"/>'
                f'<text x="366" y="{my + 34}" class="mono">Auto</text>'
                f'<text x="366" y="{my + 62}" class="mono">Cool</text>'
                f'<text x="366" y="{my + 90}" class="mono">Heat</text></g>',
                f'<text x="600" y="{my + 5}" class="z">'
                f'options 里每项带 labels 映射</text>',
            ]
        elif key == "text":
            t += [
                f'<rect x="352" y="{my - 16}" width="220" height="32" rx="8" '
                f'class="box"/>',
                f'<text x="366" y="{my + 5}" class="mono" style="{anim}'
                f'animation-name:rA">23.4 °C</text>',
                f'<text x="366" y="{my + 5}" class="mono" style="{anim}'
                f'animation-name:rB">23.5 °C</text>',
                f'<text x="366" y="{my + 5}" class="mono" style="{anim}'
                f'animation-name:rC">23.6 °C</text>',
                f'<text x="600" y="{my + 5}" class="z">'
                f'只读参数只显示数值（access: R）</text>',
            ]
        elif key == "button":
            for j, label in enumerate(("Pause", "Next")):
                bx = 352 + j * 110
                t.append(f'<g class="press" style="{anim}'
                         f'animation-name:press{"A" if j == 0 else "B"};'
                         f'transform-box:fill-box;transform-origin:center">'
                         f'<rect x="{bx}" y="{my - 16}" width="96" height="32" '
                         f'rx="9" class="btn"/>'
                         f'<text x="{bx + 48}" y="{my + 5}" text-anchor="middle" '
                         f'class="mono">{label}</text></g>')
            t.append(f'<text x="600" y="{my + 5}" class="z">'
                     f'命令型参数：点击即下发，无值</text>')
        else:  # hidden
            t += [
                f'<rect class="ghost" x="352" y="{my - 16}" width="220" '
                f'height="32" rx="8" fill="none" stroke="var(--line)" '
                f'stroke-dasharray="5 4" style="{anim}animation-name:ghost"/>',
                f'<text x="366" y="{my + 5}" class="z">'
                f'不渲染 · not rendered</text>',
                f'<text x="600" y="{my + 5}" class="z">'
                f'实例 visible: false 藏起这台设备的这个参数</text>',
            ]

    t += [
        f'<text x="40" y="{foot_y}" class="cap">'
        '色相 / 饱和度 / 色温滑杆的色带来自按参数名推断的 tone，不是一个单独的控件类型。</text>',
        "</svg>",
    ]
    return "\n".join(t)


def stage_data_model_figures() -> Path:
    _STAGED.mkdir(parents=True, exist_ok=True)
    for lang in ("en", "zh_CN"):
        write_text(_STAGED / f"point_lifecycle.{lang}.svg", _lifecycle_svg(lang))
    (_STAGED / "point_lifecycle.svg").unlink(missing_ok=True)
    write_text(_STAGED / "widget_styles.svg", _widgets_svg())
    return _STAGED


def chip_role(name, rawtext, text, lineno, inliner, options=None, content=None):
    """Inline ``:chip:`light``` role: a colored pill, stable per word.

    The color comes from a fixed 10-step palette chosen by the word itself, so the
    same type or widget always gets the same hue without a lookup table. Used by
    the generated type catalog so a reader can scan a column by color.
    """
    options = options or {}
    content = content or []
    key = "".join(ch for ch in text.lower() if ch.isalnum()) or "x"
    idx = sum(ord(c) for c in key) % 10
    node = nodes.inline(rawtext, text, classes=["mh-chip", f"mh-chip-{idx}"], **options)
    return [node], []


class CatalogTable(Directive):
    required_arguments = 1
    optional_arguments = 0
    final_argument_whitespace = False
    has_content = False

    def run(self):
        what = self.arguments[0].strip().lower()
        lang = self.state.document.settings.env.config.language
        if what == "devices":
            lines = devices_rst(lang)
        elif what in ("parameters", "params"):
            lines = parameters_rst(lang)
        else:
            raise self.error("unknown catalog table %r (use devices or parameters)" % what)
        node = nodes.Element()
        self.state.nested_parse(StringList(lines), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-catalog", CatalogTable)
    app.add_role("chip", chip_role)

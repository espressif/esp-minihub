# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage the layered architecture figure: static SVG plus an interactive page.

Sphinx ``html_static_path`` ships ``_static/architecture/``. The figures are
generated from the single ``LAYERS`` table below, so the picture, the
interactive page and the captions cannot drift apart. Do not edit staged copies.

Keep the data honest: every label here must be true of the code it names, and
L3 rows are dropped for the ``hide_l3`` documentation build.

Motion design: the bands are a timed walkthrough top-to-bottom (entry points
first, silicon last), so the picture reads in the same order the prose does,
while the two shafts on the left keep flowing continuously — red downward for
commands, cyan upward for state. That split is deliberate: the tour explains
the stack, the shafts explain the circulation. All motion is CSS, so it
survives ``<img>`` embedding and stops under ``prefers-reduced-motion``.
"""

from __future__ import annotations

from pathlib import Path
from xml.sax.saxutils import escape as _esc

from sphinx.errors import ExtensionError

from minihub_figure_kit import text_width as _tw, wrap_words as _wrap_words, write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "architecture"
_REPO = _DOCS_DIR.parent
_MONO = "IBM Plex Mono, Menlo, monospace"

# One dict per band. Keys: id, en/zh title, nodes[(en, zh, note, dirs)], note,
# ``dir_prefix`` shared by the band's directories (printed once, on the title
# row, so a card only has to carry the part that differs), and the optional flags
# ``l3_only`` (dropped from the published build) and ``external`` (drawn outside
# the Host: dashed panel plus a "Hub 之外" tag). ``check()`` refuses to stage a
# band whose directories no longer exist, so the paths cannot quietly fall behind
# a rename.
LAYERS: list[dict] = [
    dict(
        id="entry", en="Entry points", zh="入口", dir_prefix="components/",
        nodes=[("Web console", "Web 控制台", "OEM-skinnable, read from /data/web", ("minihub_web",)),
               ("REST :80", "REST :80", "70 routes + WebSocket /ws/events", ("minihub_web",)),
               ("MCP server", "MCP 服务", "optional, 12 tools, off by default", ("minihub_mcp_server",))],
        note="One state model behind all three: the point bus. Nothing here is a second source of truth.",
    ),
    dict(
        id="services", en="Hub services and orchestration", zh="Hub 服务与编排", dir_prefix="components/",
        nodes=[("Hub reload", "Hub 热重载", "one sequence: stop, re-read, restart", ("minihub_hub",)),
               ("IP uplink", "IP 上行", "Ethernet first, then Wi-Fi STA", ("minihub_net",)),
               ("OTA and packages", "OTA 与包", "state machine plus the deploy-root puller", ("minihub_ota", "minihub_update")),
               ("Provisioning", "配网与恢复出厂", "SoftAP credentials, factory reset", ("minihub_provision",)),
               ("RainMaker bridge", "RainMaker 桥", "linked in only when the option is on", ("minihub_rainmaker",))],
        note="Called by the console and MCP, and calling down into everything below. minihub_net is also where the weak hooks live: it ships no-op stubs that the web, LCD and RainMaker builds override when they are linked in.",
    ),
    dict(
        id="clouds", en="Clouds", zh="云", external=True,
        nodes=[("Vendor cloud", "厂商云（后装）", "MIoT and the like: one installed module", ()),
               ("RainMaker cloud", "RainMaker 云", "the endpoint the bridge above reaches", ())],
        note="Endpoints, not layers: neither is a build dependency. What differs is how the speaking code arrived — a module installed at run time, or the bridge linked in by CONFIG_MINIHUB_RAINMAKER.",
    ),
    dict(
        id="core", en="Type-agnostic core", zh="不认协议的核心层", dir_prefix="components/",
        nodes=[("minihub_point_t", "点位与事件总线", "one device = one point; props.params per parameter", ("minihub_core",)),
               ("Type catalog", "类型目录", "CSV is the single source; a new type needs no C", ("minihub_device_widget",)),
               ("Config files", "配置文件", "reads and writes /data/config/*.json", ("minihub_config",)),
               ("Adapter registry", "适配器注册表", "start order, lifecycle lock, poll task", ("minihub_adapter",)),
               ("Rules & scenes", "规则与场景", "on-chip, two docs, separate dispatch", ("minihub_rules", "minihub_scenes")),
               ("Auth", "认证", "mh_sess cookie or Bearer token", ("minihub_auth",))],
        note="The row that knows no protocol: core, the catalog and auth call nothing else in this tree, and the other three call only them. Adapters are handed to it by registration, not by it reaching out.",
    ),
    dict(
        id="store", en="Runtime extension (.mhub)", zh="运行时扩展（.mhub）", dir_prefix="components/",
        nodes=[("Store / loader", "Store 与加载器", "install, ABI gate, load guard", ("minihub_store", "minihub_module")),
               ("L1 pack", "L1 JSON 包", "configuration only, merged into adapters.json", ("minihub_pack",)),
               ("L2 QuickJS", "L2 QuickJS", "JavaScript on the embedded engine", ("minihub_js",))],
        note="Added after the image is flashed: nothing here is linked in, so it is installed, upgraded and enabled against a running Host. It extends what the Hub can speak without becoming a layer of it.",
    ),
    dict(
        id="store3", en="", zh="", l3_only=True, dir_prefix="components/",
        nodes=[("L3 native .so", "L3 原生 .so", "MHMD + Host export allowlist + ABI vtable", ("minihub_module",))],
        note="Same process as the Host; radio APIs and httpd URI registration are not exported.",
    ),
    dict(
        id="protocol", en="Protocol side: stack and adapter", zh="协议侧：栈与适配器", dir_prefix="components/",
        nodes=[("Wi-Fi STA", "Wi-Fi STA", "uplink only, registers no points", ("adapter_wifi",)),
               ("Zigbee", "Zigbee", "coordinator; one node = one point", ("minihub_zigbee", "adapter_zigbee")),
               ("BLE Mesh", "BLE Mesh", "provision; OnOff/Level/Lightness/CTL", ("minihub_ble_mesh", "adapter_ble_mesh")),
               ("Matter", "Matter", "controller; one endpoint = one point", ("minihub_matter", "adapter_matter")),
               ("Thread", "Thread", "border router; registers no points", ("minihub_thread", "adapter_thread")),
               ("MQTT", "MQTT", "config-only, Topic <-> Point", ("adapter_mqtt",)),
               ("Modbus", "Modbus", "master polling, TCP / RTU", ("adapter_modbus",))],
        note="A protocol is usually two directories: the stack under components/ and its adapter under components/minihub_protocol_adapters/ (Wi-Fi, MQTT and Modbus have no stack of their own). They are not one layer: MQTT and Modbus call only Core, Zigbee and BLE Mesh also call their stack and the loader. Ethernet is not an adapter; Zigbee and Thread never start together.",
    ),
    dict(
        id="base", en="Silicon and build", zh="芯片与构建", dir_prefix="",
        nodes=[("ESP32-S31", "ESP32-S31", "RISC-V, --preview target", ()),
               ("ESP-IDF 6.2", "ESP-IDF 6.2", "pinned revision in Getting Started", ()),
               ("application/<product>", "application/<产品>", "manifest.yml + configs/*.yml",
                ("application/minihub",))],
        note="A product is a configuration of this tree, not a fork of Core.",
    ),
]

# One band of the timed walkthrough, in seconds. The full cycle is
# ``STEP_S * len(layers)``; every animation below shares that cycle and is
# offset by ``i * STEP_S`` so band i lights alone.
STEP_S = 1.05

CSS = """
:root{--ink:#14202e;--muted:#54616e;--line:rgba(20,32,46,.16);--panel:#fff;--base:#f6f8fa;
--red:#e8362d;--cyan:#1c8fb8;--viz:#0ca678;--amber:#c77f0a;--violet:#7c5cff}
@media (prefers-color-scheme:dark){:root{--ink:#e6edf3;--muted:#9fb0c0;--line:rgba(230,237,243,.18);
--panel:#161b22;--base:#0d1117}}
@keyframes spot{0%{opacity:0}1.5%{opacity:.8}12%{opacity:.8}14.5%{opacity:0}100%{opacity:0}}
@keyframes breath{0%{transform:scale(1)}1.5%{transform:scale(1.006)}12%{transform:scale(1.006)}
14.5%{transform:scale(1)}100%{transform:scale(1)}}
@keyframes bar{0%{opacity:.45}1.5%{opacity:1}12%{opacity:1}14.5%{opacity:.45}100%{opacity:.45}}
/* shafts keep flowing while the tour walks the bands */
@keyframes marchV{to{stroke-dashoffset:-24}}
@keyframes marchVBack{to{stroke-dashoffset:24}}
@keyframes goDown{0%{transform:translateY(0);opacity:0}10%{opacity:1}90%{opacity:1}
100%{transform:translateY(var(--span));opacity:0}}
@keyframes goUp{0%{transform:translateY(var(--span));opacity:0}10%{opacity:1}90%{opacity:1}
100%{transform:translateY(0);opacity:0}}
.ring{fill:none;stroke-width:2;opacity:0;transform-box:fill-box;transform-origin:center}
.halo{fill:none;stroke-width:7;opacity:0}
.bar{opacity:.45}
.wash{opacity:0}
.shaft{fill:none;stroke-width:2;stroke-linecap:round;stroke-dasharray:5 7;opacity:.55}
.halo-dot{opacity:.22}
@media (prefers-reduced-motion: reduce){
  .ring,.halo,.bar,.wash,.shaft,.dot,.dot2{animation:none}
  .shaft{opacity:.4}.dot,.dot2{display:none}.bar{opacity:.8}.ring,.halo,.wash{opacity:0}
}
"""

ACCENT = ("#e8362d", "#1c8fb8", "#0ca678", "#c77f0a", "#7c5cff", "#e8362d", "#1c8fb8")


def _svg(hide_l3: bool) -> str:
    """Layered figure sized by its own text, so nothing is truncated or overlapped.

    One LAYERS row per band; band height, card height and the number of wrapped
    note lines all come from measured text, so picture and prose cannot drift and
    a longer note can never collide with the card below it. The timed band
    walkthrough (see module docstring) rides on top of that measured layout.
    """
    layers = [l for l in LAYERS if not (hide_l3 and l.get("l3_only"))]
    w, m = 1180, 40
    inner, pad, gap = w - 2 * m, 18, 12
    top = 112
    # one type scale: a size change moves the boxes with it instead of clipping text
    SZ = dict(lab=16, labzh=15, nm=16, zh=14, nt=12.5, bn=13.5, dr=11)
    LH = dict(nm=19, zh=17, nt=15, bn=18, dr=14)

    spec = []
    for lay in layers:
        n = len(lay["nodes"])
        cw = (inner - 2 * pad - (n - 1) * gap) / n
        texts = []
        for en, zh, note, dirs in lay["nodes"]:
            texts.append((_wrap_words(en, SZ["nm"], cw - 22, 3),
                          _wrap_words(zh, SZ["zh"], cw - 22, 3),
                          _wrap_words(" + ".join(dirs), SZ["dr"], cw - 22, 2) if dirs else [],
                          _wrap_words(note, SZ["nt"], cw - 22, 5)))
        dmax = max(len(x[2]) for x in texts)
        card_h = (30 + max(len(x[0]) for x in texts) * LH["nm"]
                  + max(len(x[1]) for x in texts) * LH["zh"] + 6
                  + (dmax * LH["dr"] + 4 if dmax else 0)
                  + max(len(x[3]) for x in texts) * LH["nt"] + 10)
        note_lines = _wrap_words(lay["note"], SZ["bn"], inner - 2 * pad - 10, 3)
        band_h = pad + 26 + 12 + card_h + 10 + len(note_lines) * LH["bn"] + pad
        spec.append(dict(lay=lay, texts=texts, cw=cw, card_h=card_h,
                         note_lines=note_lines, band_h=band_h))

    h = top + sum(s["band_h"] + 16 for s in spec) + 20
    cycle = STEP_S * len(spec)

    t = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h:.0f}" '
         f'viewBox="0 0 {w} {h:.0f}" '
         'font-family="IBM Plex Sans, Noto Sans CJK SC, sans-serif" role="img">',
         "<title>MiniHub layered architecture / MiniHub 分层架构</title>",
         f"<style>{CSS}</style>",
         '<defs>'
         '<marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
         'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#e8362d"/></marker>'
         '<marker id="b" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
         'markerHeight="6" orient="auto"><path d="M0 0 10 5 0 10z" fill="#1c8fb8"/></marker>'
         '<filter id="soft" x="-20%" y="-20%" width="140%" height="140%">'
         '<feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#14202e" '
         'flood-opacity="0.10"/></filter>']
    for i, ac in enumerate(ACCENT[:len(spec)]):
        t.append(f'<linearGradient id="wash{i}" x1="0" y1="0" x2="1" y2="1">'
                 f'<stop offset="0" stop-color="{ac}" stop-opacity=".10"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".02"/></linearGradient>'
                 f'<linearGradient id="bar{i}" x1="0" y1="0" x2="0" y2="1">'
                 f'<stop offset="0" stop-color="{ac}"/>'
                 f'<stop offset="1" stop-color="{ac}" stop-opacity=".55"/></linearGradient>')
    t.append("</defs>")

    t.append(f'<rect width="{w}" height="{h:.0f}" fill="var(--base)"/>')
    t.append(f'<rect width="{w}" height="{top - 24}" fill="var(--panel)" opacity=".55"/>')
    t.append(f'<text x="{m}" y="46" fill="var(--ink)" font-size="23" font-weight="700">'
             + _esc("MiniHub 分层架构")
             + '<tspan style="fill:var(--muted)" font-weight="500" dx="14" font-size="15">'
             + _esc("layered architecture") + "</tspan></text>")
    t.append(f'<text x="{m}" y="74" style="fill:var(--muted)" font-size="13.5">'
             + _esc("红 = 命令下行   青 = 状态上行   紫 = 扩展向上   "
                    "red = command down · cyan = state up · violet = extension upward")
             + "</text>")

    y = top
    for idx, s in enumerate(spec):
        lay, bh = s["lay"], s["band_h"]
        accent = ACCENT[idx % len(ACCENT)]
        d = idx * STEP_S
        # an external band is a dashed panel: it is not a layer of the Host
        dash = ' stroke-dasharray="7 5"' if lay.get("external") else ""
        # band panel + wash that brightens while this band owns the spotlight
        t.append(f'<g filter="url(#soft)"><rect x="{m}" y="{y:.0f}" width="{inner}" '
                 f'height="{bh:.0f}" rx="12" fill="var(--panel)" stroke="var(--line)"{dash}/>')
        t.append(f'<rect class="wash" x="{m}" y="{y:.0f}" width="{inner}" height="{bh:.0f}" '
                 f'rx="12" fill="url(#wash{idx % len(ACCENT)})" '
                 f'style="animation:spot {cycle:.2f}s linear infinite;'
                 f'animation-delay:{d:.2f}s"/></g>')
        # spotlight ring + soft halo
        t.append(f'<rect class="halo" x="{m - 5:.0f}" y="{y - 5:.0f}" width="{inner + 10:.0f}" '
                 f'height="{bh + 10:.0f}" rx="17" stroke="{accent}" '
                 f'style="animation:spot {cycle:.2f}s linear infinite;'
                 f'animation-delay:{d:.2f}s"/>')
        t.append(f'<rect class="ring" x="{m - 3:.0f}" y="{y - 3:.0f}" width="{inner + 6:.0f}" '
                 f'height="{bh + 6:.0f}" rx="15" stroke="{accent}" '
                 f'style="animation:spot {cycle:.2f}s linear infinite,breath {cycle:.2f}s '
                 f'ease-in-out infinite;animation-delay:{d:.2f}s,{d:.2f}s"/>')
        t.append(f'<rect class="bar" x="{m}" y="{y + 14:.0f}" width="3" '
                 f'height="{bh - 28:.0f}" rx="1.5" fill="url(#bar{idx % len(ACCENT)})" '
                 f'style="animation:bar {cycle:.2f}s linear infinite;'
                 f'animation-delay:{d:.2f}s"/>')
        ty = y + pad + 12
        t.append(f'<text x="{m + pad + 10}" y="{ty:.0f}" fill="var(--ink)" font-size="16" '
                 f'font-weight="600">{_esc(lay["en"].upper())}'
                 f'<tspan style="fill:var(--muted)" font-weight="500" dx="10" '
                 f'font-size="15">{_esc(lay["zh"])}</tspan></text>')
        # one prefix per band, printed once: the cards then carry only what differs
        if lay.get("dir_prefix"):
            t.append(f'<text x="{m + inner - pad - 10:.0f}" y="{ty:.0f}" text-anchor="end" '
                     f'fill="var(--muted)" font-size="11" font-family="{_MONO}">'
                     f'{_esc(lay["dir_prefix"])}</text>')
        if lay.get("external"):
            x_tag = (m + inner - pad - 10 - _tw(lay.get("dir_prefix", ""), 11) - 18
                     if lay.get("dir_prefix") else m + inner - pad - 10)
            t.append(f'<text x="{x_tag:.0f}" y="{ty:.0f}" text-anchor="end" '
                     f'fill="var(--muted)" font-size="12">{_esc("Hub 之外 · outside the Host")}</text>')
        cy = ty + 14
        for k, (l_en, l_zh, l_dr, l_nt) in enumerate(s["texts"]):
            cx = m + pad + 10 + k * (s["cw"] + gap)
            t.append(f'<g filter="url(#soft)"><rect x="{cx:.0f}" y="{cy:.0f}" '
                     f'width="{s["cw"]:.0f}" height="{s["card_h"]:.0f}" rx="8" '
                     f'fill="var(--panel)" stroke="var(--line)"/>')
            # static per-card wash: depth without joining the spotlight
            t.append(f'<rect x="{cx:.0f}" y="{cy:.0f}" width="{s["cw"]:.0f}" '
                     f'height="{s["card_h"]:.0f}" rx="8" '
                     f'fill="url(#wash{idx % len(ACCENT)})" opacity=".5"/></g>')
            ly = cy + 23
            for seg in l_en:
                t.append(f'<text x="{cx + 10:.0f}" y="{ly:.0f}" fill="var(--ink)" '
                         f'font-size="16" font-weight="600">{_esc(seg)}</text>')
                ly += LH["nm"]
            for seg in l_zh:
                t.append(f'<text x="{cx + 10:.0f}" y="{ly:.0f}" fill="var(--muted)" '
                         f'font-size="14">{_esc(seg)}</text>')
                ly += LH["zh"]
            for seg in l_dr:
                t.append(f'<text x="{cx + 10:.0f}" y="{ly:.0f}" fill="var(--amber)" '
                         f'font-size="{SZ["dr"]}" font-family="{_MONO}">{_esc(seg)}</text>')
                ly += LH["dr"]
            ly += 7
            for seg in l_nt:
                t.append(f'<text x="{cx + 10:.0f}" y="{ly:.0f}" fill="var(--muted)" '
                         f'font-size="12.5">{_esc(seg)}</text>')
                ly += LH["nt"]
        ny = y + bh - pad - (len(s["note_lines"]) - 1) * LH["bn"] + 4
        for seg in s["note_lines"]:
            t.append(f'<text x="{m + pad + 10}" y="{ny:.0f}" fill="var(--muted)" '
                     f'font-size="13.5">{_esc(seg)}</text>')
            ny += LH["bn"]
        s["y"], s["h"] = y, bh
        y += bh + 16

    # shafts: command down (red) and state up (cyan), flowing the whole stack
    first, last = spec[0], spec[-1]
    y0, y1 = first["y"] + 20, last["y"] + last["h"] - 20
    span = y1 - y0
    t.append(f'<g style="--span:{span:.0f}px">'
             f'<path d="M16 {y0:.0f} V{y1:.0f}" stroke="var(--line)" stroke-width="2" '
             f'fill="none"/>'
             f'<path d="M30 {y1:.0f} V{y0:.0f}" stroke="var(--line)" stroke-width="2" '
             f'fill="none"/>'
             f'<path class="shaft" d="M16 {y0:.0f} V{y1:.0f}" stroke="var(--red)" '
             f'style="animation:marchV 1.1s linear infinite"/>'
             f'<path class="shaft" d="M30 {y1:.0f} V{y0:.0f}" stroke="var(--cyan)" '
             f'style="animation:marchVBack 1.1s linear infinite"/>'
             f'<circle class="halo-dot" cx="16" cy="{y0:.0f}" r="7" fill="var(--red)"/>'
             f'<circle class="halo-dot" cx="30" cy="{y0:.0f}" r="7" fill="var(--cyan)"/>'
             f'<circle class="dot" cx="16" cy="{y0:.0f}" r="3.5" fill="var(--red)" '
             f'style="animation:goDown 3.2s ease-in-out infinite"/>'
             f'<circle class="dot2" cx="30" cy="{y0:.0f}" r="3.5" fill="var(--cyan)" '
             f'style="animation:goUp 3.2s ease-in-out infinite .5s"/></g>')

    # The other upward channel, which no band can show: a lower layer naming a
    # symbol the upper build overrides (weak hooks), and the Host's own entry
    # points handed to installed code through the plugin ABI vtable.
    x_up = w - 18
    mid = (y0 + y1) / 2
    t.append(f'<g style="--span:{span:.0f}px">'
             f'<path d="M{x_up} {y1:.0f} V{y0:.0f}" stroke="var(--line)" stroke-width="2" '
             f'fill="none"/>'
             f'<path class="shaft" d="M{x_up} {y1:.0f} V{y0:.0f}" stroke="var(--violet)" '
             f'style="animation:marchVBack 1.4s linear infinite"/>'
             f'<circle class="halo-dot" cx="{x_up}" cy="{y0:.0f}" r="7" fill="var(--violet)"/>'
             f'<circle class="dot2" cx="{x_up}" cy="{y0:.0f}" r="3.5" fill="var(--violet)" '
             f'style="animation:goUp 3.2s ease-in-out infinite 1s"/>'
             f'<text x="{x_up}" y="{mid:.0f}" text-anchor="middle" transform="rotate(-90 {x_up} {mid:.0f})" '
             f'style="fill:var(--muted)" font-size="11.5">扩展向上 · extension upward</text></g>')
    t.append("</svg>")
    return "\n".join(t)


def _attr(s: str) -> str:
    return _esc(s, {'"': "&quot;"})


def _node_button(lay: dict, node: tuple) -> str:
    """One block on the interactive page: name, gloss, and the directory it is."""
    en, zh, note, dirs = node
    path = " + ".join(resolve_dir(lay, d) or d for d in dirs)
    code = '<code class="dir">%s</code>' % _esc(path) if path else ""
    return ('<button class="node" data-note="%s" data-dir="%s"><b>%s</b><i>%s</i>%s</button>'
            % (_attr(note), _attr(path), _esc(en), _esc(zh), code))


def _html(hide_l3: bool) -> str:
    layers = [l for l in LAYERS if not (hide_l3 and l.get("l3_only"))]
    cards = "".join(
        '<article class="band%s" data-id="%s"><h3>%s <span>%s</span>'
        '%s</h3><div class="nodes">%s</div>'
        '<p class="note">%s</p></article>'
        % (" external" if lay.get("external") else "", lay["id"], lay["en"], lay["zh"],
           '<em class="tag">Hub 之外 · outside the Host</em>' if lay.get("external") else "",
           "".join(_node_button(lay, n) for n in lay["nodes"]),
           lay["note"])
        for lay in layers)
    step_ms = int(STEP_S * 1000)
    # Placeholders, not %-format: the HTML template carries its own CSS with
    # percent signs (``height:100%``), which %-formatting would choke on.
    html = ("""<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MiniHub 分层架构 · layered architecture</title><style>""" + CSS + """
body{margin:0;padding:28px;background:var(--base);color:var(--ink);font:16.5px/1.65 IBM Plex Sans,"Noto Sans CJK SC",sans-serif}
h1{font-size:25px;margin:0 0 6px}p.lead{color:var(--muted);margin:0 0 14px;font-size:14.5px;max-width:74ch}
.tour{height:3px;border-radius:2px;background:var(--line);overflow:hidden;margin:0 0 22px}
.tour>i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--red),var(--cyan),var(--viz),var(--amber));
animation:fill linear infinite}
@keyframes fill{from{width:0}to{width:100%}}
.band{border:1px solid var(--line);border-radius:12px;background:var(--panel);padding:14px 16px;margin:0 0 14px;
transition:box-shadow .25s,transform .25s,border-color .25s;cursor:default}
.band.focus{border-color:var(--red);box-shadow:0 0 0 2px var(--red) inset,0 6px 18px rgba(20,32,46,.08);transform:translateY(-2px)}
.band.external{border-style:dashed}
.band .tag{float:right;font-style:normal;color:var(--muted);font-size:12px}
.band h3{margin:0 0 12px;font-size:16px}
.band h3 span{color:var(--muted);font-weight:400;font-size:15px;margin-left:10px}
.nodes{display:flex;flex-wrap:wrap;gap:8px}
.node{border:1px solid var(--line);background:var(--base);color:var(--ink);border-radius:8px;
padding:9px 12px;text-align:left;font:inherit;cursor:pointer;transition:border-color .2s,transform .2s}
.node:hover,.node:focus-visible{border-color:var(--red);transform:translateY(-1px)}
.node b{display:block;font-size:15px}.node i{display:block;font-style:normal;font-size:13px;color:var(--muted)}
.node .dir{display:block;margin-top:4px;font:11.5px IBM Plex Mono,Menlo,monospace;color:var(--amber)}
.note{color:var(--muted);font-size:13.5px;margin:12px 0 0}
#say{position:sticky;bottom:0;background:var(--panel);border:1px solid var(--line);border-radius:10px;
padding:12px 16px;margin-top:18px;font-size:14.5px;min-height:1.6em}
@media (max-width:720px){.band h3 span{display:block;margin:2px 0 0}}
</style></head><body>
<h1>MiniHub 分层架构 · layered architecture</h1>
<p class="lead">各层自上而下自动导览；点任意一块看它承担的约束，Esc 取消高亮。<br>
The bands tour top to bottom on their own; click a block for the constraint it carries. Escape clears.</p>
<div class="tour"><i id="bar" style="animation-duration:__CYCLE__ms"></i></div>
<main id="bands">__CARDS__</main><p id="say">本页不请求外部资源，离线可读。</p>
<script>
(function(){
  var bands=[].slice.call(document.querySelectorAll('.band'));
  var say=document.getElementById('say'), bar=document.getElementById('bar');
  var i=0, timer=null;
  var still=window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  function show(k){
    bands.forEach(function(b,j){b.classList.toggle('focus',j===k)});
    say.textContent=bands[k].querySelector('.note').textContent;
  }
  function play(){ if(still) return; timer=setInterval(function(){i=(i+1)%bands.length;show(i)}, __STEP__); }
  function pause(){ if(timer){clearInterval(timer);timer=null;} }
  show(0);
  if(!still){
    play();
    document.getElementById('bands').addEventListener('mouseenter',pause);
    document.getElementById('bands').addEventListener('mouseleave',play);
  }
  document.querySelectorAll('.node').forEach(function(n){
    n.addEventListener('click',function(){
      pause();
      bands.forEach(function(b){b.classList.toggle('focus',b===n.closest('.band'))});
      say.textContent=n.dataset.note+(n.dataset.dir?' — '+n.dataset.dir:'');
    });
  });
  document.addEventListener('keydown',function(e){if(e.key==='Escape'){
    bands.forEach(function(b){b.classList.remove('focus')});say.textContent='\\u2014';play();}});
})();
</script></body></html>
""")
    return (html.replace("__CARDS__", cards)
                .replace("__STEP__", str(step_ms))
                .replace("__CYCLE__", str(step_ms * len(layers))))


def check() -> list[str]:
    """Every directory the figure names must exist, or the picture is a lie.

    A reader who follows a box here expects to find that folder; a rename that
    leaves one behind is exactly the drift a figure cannot report by itself.
    """
    problems = []
    for lay in LAYERS:
        for en, _zh, _note, dirs in lay["nodes"]:
            for d in dirs:
                if resolve_dir(lay, d) is None:
                    problems.append(f"{lay['id']}: {en} cites {d}, "
                                    "which is not a directory in this tree")
    return problems


def resolve_dir(lay: dict, name: str) -> str | None:
    """The repo path of a card's directory, or ``None`` if there is none.

    A card shows the short name (the seven protocol cards would not fit any
    longer), so the full path is resolved here once and reused by the gate and
    by the interactive page's caption.
    """
    for root in (lay.get("dir_prefix", ""), "components/",
                 "components/minihub_protocol_adapters/", ""):
        if root and name.startswith(root):
            cand = name
        else:
            cand = root + name
        if (_REPO / cand).is_dir():
            return cand
    return None


def stage_architecture_diagram(hide_l3: bool = False) -> Path:
    """Write the figure pair for this documentation variant.

    The file names do not follow the flag: a build that publishes no native tier
    should not name one in its asset URLs either.
    """
    problems = check()
    if problems:
        raise ExtensionError("stage_architecture is out of date:\n  "
                             + "\n  ".join(problems))
    _STAGED.mkdir(parents=True, exist_ok=True)
    svg = _STAGED / "layered.svg"
    html = _STAGED / "index.html"
    write_text(svg, _svg(hide_l3))
    write_text(html, _html(hide_l3))
    # The suffixed names were an earlier scheme; a leftover copy rides into the
    # published tree through html_static_path and stays reachable by URL.
    for stale in (_STAGED / "layered_no_l3.svg", _STAGED / "index_no_l3.html"):
        stale.unlink(missing_ok=True)
    return _STAGED

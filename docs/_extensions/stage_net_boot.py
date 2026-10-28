# SPDX-FileCopyrightText: 2026 MiniHub Contributors
#
# SPDX-License-Identifier: Apache-2.0
"""Stage the network bring-up figure.

One BOOT_STAGES table drives both languages of the inline SVG and the interactive
page. Each language gets its own file: no Chinese prose in the English build, no
English prose in the Chinese build, while function and route names stay as the
code spells them. Keep every name in the table true to the code it names:

* ``minihub_net_start()`` and the credential read — components/minihub_net/minihub_net.c
* the open-SoftAP fallback (``PROV_MiniHub``, channel 1, port 80) — same file
* ``POST /api/net/wifi`` — minihub_web.c route registration
* ``minihub_net_refresh_default_netif()`` — the default-route choice the guide
  tells plugin authors to rely on
"""

from __future__ import annotations

from pathlib import Path

from minihub_figure_kit import interactive_page, svg_figure, write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "net_boot"
# Distinct from the other flow files: Sphinx copies every .. image:: target into
# _images/ keyed by basename, and same-named figures become flow2.svg there.
SVG_BASE = "boot_flow"

TXT = {
    "zh": {
        "title": "MiniHub 网络起来的那几秒",
        "lead": "开机后五步：先起以太网 → 轮到 Wi-Fi 适配器 → 连 STA 拿地址 → 选默认路由 → 控制台上线",
        "return": ("青虚线 = 没有凭据：开一个开放 SoftAP，连上它打开 80 端口的控制台，"
                   "POST /api/net/wifi 存好凭据后回到 minihub_net_start_wifi()"),
        "legend": ("没有凭据时 Host 起的是开放 SoftAP（SSID PROV_MiniHub，信道 1），"
                   "只为把控制台暴露出来配网",),
        "page_lead_end": "。卡片自动逐站导览，悬停暂停。",
        "offline": "本页不请求外部资源，离线可读。",
    },
    "en": {
        "title": "MiniHub network bring-up",
        "lead": ("Five steps after power-on: Ethernet up first → the Wi-Fi adapter → "
                 "connect as STA and get an address → pick the default route → the "
                 "console comes up"),
        "return": ("Cyan dashed = no credentials: an open SoftAP comes up, connect to "
                   "it and reach the console on port 80; POST /api/net/wifi stores the "
                   "credentials and control returns to minihub_net_start_wifi()"),
        "legend": ("Without credentials the Host starts an open SoftAP (SSID "
                   "PROV_MiniHub, channel 1) only to expose the console for "
                   "provisioning",),
        "page_lead_end": ". The cards tour themselves and hover pauses the tour.",
        "offline": "This page fetches no external resource and works offline.",
    },
}

BOOT_STAGES = [
    # (code, label zh, actor, note zh, note en)
    ("minihub_net_init", "先把以太网叫醒", "netif + EMAC",
     "建 netif 与事件循环，以太网直接 start；Wi-Fi 只注册处理（日志 wifi STA deferred）",
     "Creates the netif and the event loop; Ethernet starts at once while Wi-Fi only "
     "registers its handler (the log reads wifi STA deferred)"),
    ("minihub_net_start_wifi", "轮到 Wi-Fi", "adapter order",
     "射频互斥后按 Wi-Fi → Zigbee 或 Thread → BLE Mesh 的顺序启动适配器",
     "Once the radio mutex allows it, adapters start in the order Wi-Fi → Zigbee or "
     "Thread → BLE Mesh"),
    ("esp_wifi_connect", "连 STA", "NVS creds",
     "照着保存过的凭据连，连上由 netif 上报 IPv4 与网关",
     "Connects using the stored credentials; on association the netif reports the "
     "IPv4 address and gateway"),
    ("refresh_default_netif", "选默认出口", "eth first",
     "以太网有 IPv4 就走以太网，否则走有 IPv4 的 Wi-Fi",
     "Ethernet takes the default route when it has IPv4, otherwise it goes to the "
     "Wi-Fi that has one"),
    ("httpd on :80", "控制台上线", "web · REST · MCP",
     "局域网里打开 http://HUB/ 就能用",
     "Open http://HUB/ on the LAN and the console is usable"),
]


def stage_net_boot_diagram() -> Path:
    """Write both languages of the bring-up figure and its tour page."""
    _STAGED.mkdir(parents=True, exist_ok=True)
    for lang in ("en", "zh_CN"):
        key = "en" if lang == "en" else "zh"
        txt = TXT[key]
        write_text(_STAGED / f"{SVG_BASE}.{lang}.svg",
                   svg_figure(txt["title"], "", txt["lead"], BOOT_STAGES,
                              txt["legend"], return_from=2, return_to=1,
                              return_caption=txt["return"], lang=key))
        write_text(_STAGED / lang / "index.html",
                   interactive_page(
                       txt["title"], txt["lead"] + txt["page_lead_end"],
                       BOOT_STAGES, txt["offline"], lang=key))
    (_STAGED / "boot_flow.svg").unlink(missing_ok=True)
    (_STAGED / "flow.svg").unlink(missing_ok=True)
    (_STAGED / "index.html").unlink(missing_ok=True)
    return _STAGED

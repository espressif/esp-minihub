# SPDX-FileCopyrightText: 2026 MiniHub Contributors
#
# SPDX-License-Identifier: Apache-2.0
"""Stage the rules evaluation figure: SVG plus an interactive page.

One STAGES table drives both languages, so figure, page and captions cannot drift
apart; do not edit staged copies. Each language gets its own file — the English
build carries no Chinese prose and the Chinese build no English prose — while
identifiers (``point.updated``, ``delay_after``, the action kinds) stay as the
code spells them.

Motion: a timed walkthrough, one slot per stage (the kit's STEP_S). Stage i, the connector into it
and its comet share one cycle with per-stage delays, so the figure tours the
evaluation in prose order; the cyan return lights when the actions stage is
active, because a command action writes a point and the resulting point.updated
starts the next round. All motion is CSS.
"""

from __future__ import annotations

from pathlib import Path

from minihub_figure_kit import interactive_page, svg_figure, write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "rules"
SVG_BASE = "rules_flow"

TXT = {
    "zh": {
        "title": "MiniHub 规则求值",
        "lead": "一次点位更新触发规则：点位更新 → 触发 → 附加条件 → 步骤 → 动作",
        "legend": ("箭头向右 = 规则求值的前进方向；青虚线向左 = command 动作写入点位，"
                   "开启下一轮",),
        "page_lead_end": "。卡片自动逐站导览，悬停暂停。",
        "offline": "本页不请求外部资源，离线可读。",
    },
    "en": {
        "title": "MiniHub rules evaluation",
        "lead": ("One point update triggers a rule: point updated → trigger → extra "
                 "conditions → steps → actions"),
        "legend": ("Arrows right = the direction evaluation moves; cyan dashed left = "
                   "a command action writes a point, starting the next round",),
        "page_lead_end": ". The cards tour themselves and hover pauses the tour.",
        "offline": "This page fetches no external resource and works offline.",
    },
}

STAGES = [
    # (code, label zh, actor, note zh, note en)
    ("point.updated", "点位更新", ("事件总线", "event bus"),
     "值变了才在总线上广播", "A changed value is broadcast on the bus"),
    ("when", "触发", ("边沿触发", "edge-triggered"),
     "进入命中状态就把规则触发一次", "Entering the hit state fires the rule once"),
    ("if", "附加条件", ("最多 4 条，全部成立", "max 4, all must hold"),
     "与在线点位比较；if 自己不触发", "Compared to live points; if never triggers"),
    ("then", "步骤", ("有序步骤", "ordered steps"),
     "步骤按顺序跑，每步先等 delay_after",
     "Steps run in order, each after delay_after"),
    ("actions", "动作", "command / notify",
     "command 走同一条命令路由", "command goes through the same command router"),
]

RET_FROM = 4


def stage_rules_diagram() -> Path:
    """Write both languages of the rules figure and its tour page."""
    _STAGED.mkdir(parents=True, exist_ok=True)
    for lang in ("en", "zh_CN"):
        key = "en" if lang == "en" else "zh"
        txt = TXT[key]
        write_text(_STAGED / f"{SVG_BASE}.{lang}.svg",
                   svg_figure(txt["title"], "", txt["lead"], STAGES, txt["legend"],
                              return_from=RET_FROM, lang=key))
        write_text(_STAGED / lang / "index.html",
                   interactive_page(
                       txt["title"], txt["lead"] + txt["page_lead_end"],
                       STAGES, txt["offline"], lang=key))
    (_STAGED / "rules_flow.svg").unlink(missing_ok=True)
    # Sphinx keys _images/ by basename: two figures called flow.svg come out as
    # flow.svg plus flow1.svg, so a URL no longer says which figure it is.
    (_STAGED / "flow.svg").unlink(missing_ok=True)
    (_STAGED / "index.html").unlink(missing_ok=True)
    return _STAGED

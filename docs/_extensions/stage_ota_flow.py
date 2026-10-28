# SPDX-FileCopyrightText: 2026 MiniHub Contributors
#
# SPDX-License-Identifier: Apache-2.0
"""Stage the Hub firmware-update figure.

One UPDATE_STAGES table drives both languages of the inline SVG and the
interactive page, so the figure, the page and the captions cannot drift apart.
Each language gets its own file: the English build contains no Chinese prose and
the Chinese build no English prose, while identifiers (routes, function names)
stay as the code spells them. Keep every number in the table true to the code it
names:

* ``/api/update/{hints,notes,check,apply}`` — minihub_web.c route registration
* ``esp_ota_begin`` / ``esp_ota_end`` / ``esp_ota_set_boot_partition``
  and the 800 ms delay before ``esp_restart()`` — minihub_ota.c
* the pending-verify confirmation — ``esp_ota_mark_app_valid_cancel_rollback()``
"""

from __future__ import annotations

from pathlib import Path

from minihub_figure_kit import interactive_page, svg_figure, write_text

_DOCS_DIR = Path(__file__).resolve().parents[1]
_STAGED = _DOCS_DIR / "_static" / "ota_update"
# Distinct from the other flow files: Sphinx copies every .. image:: target into
# _images/ keyed by basename, and same-named figures become flow2.svg there.
SVG_BASE = "update_flow"

TXT = {
    "zh": {
        "title": "MiniHub 固件更新",
        "lead": "一次更新六步：读提示 → 后台重探 → 交 URL → 写空闲槽 → 校验 → 换槽重启并确认",
        "return": ("青虚线 = 校验或确认没过去：bootloader 回滚到另一张镜像槽，"
                   "下一轮重新读提示"),
        "legend": ("箭头向右 = 更新的前进方向；青虚线向左 = 回滚，回到另一张镜像槽",),
        "page_lead_end": "。卡片自动逐站导览，悬停暂停。",
        "offline": "本页不请求外部资源，离线可读。",
    },
    "en": {
        "title": "MiniHub firmware update",
        "lead": ("Six steps per update: read the hints → re-probe off-task → hand over "
                 "the URL → write the free slot → verify → switch slot, restart, confirm"),
        "return": ("Cyan dashed = the verify or the confirmation did not pass: the "
                   "bootloader rolls back to the other image slot and the next round "
                   "reads the hints again"),
        "legend": ("Arrows right = the direction an update moves; cyan dashed left = "
                   "rollback, back to the other image slot",),
        "page_lead_end": ". The cards tour themselves and hover pauses the tour.",
        "offline": "This page fetches no external resource and works offline.",
    },
}

UPDATE_STAGES = [
    # (code, label zh, actor, note zh, note en)
    ("cached probe", "读更新提示", "GET /api/update/hints",
     "控制台读 Hub 缓存的探测结果：固件、控制台、资源各有没有新版",
     "The console reads the Hub's cached probe: firmware, console and assets each "
     "have a newer version or not"),
    ("off-task", "后台重探", "POST /api/update/check",
     "探测不占 httpd 任务，立刻返回 pending；GET 轮询到 idle 为止",
     "The probe does not occupy an httpd task and answers pending at once; GET polls "
     "until it reads idle"),
    ("start download", "交 URL", "POST /api/update/apply",
     "body 只带 type / force；Host 用配置里的部署 URL，补 mac= 与 cur_version=",
     "The body carries only type / force; the Host takes the deploy URL from config "
     "and appends mac= and cur_version="),
    ("esp_ota_begin", "写空闲槽", "write OTA firmware",
     "正在跑的那张槽保持不动，另一张（ota_0 / ota_1）接收新镜像",
     "The running slot stays untouched; the other one (ota_0 / ota_1) receives the "
     "new image"),
    ("esp_ota_end", "校验镜像", "header + checksum",
     "校验不过直接丢弃，启动槽不改写，设备继续跑原版本",
     "A failed check discards the image, the boot slot is not rewritten and the "
     "device keeps running the old version"),
    ("set_boot_partition", "换槽重启", "800 ms → restart",
     "切完启动槽等 800 ms 再 esp_restart()；新镜像起来后取消回滚标记",
     "After switching the boot slot it waits 800 ms, then calls esp_restart(); the "
     "new image clears the rollback marker once it is up"),
]


def stage_ota_update_diagram() -> Path:
    """Write both languages of the OTA figure and its tour page."""
    _STAGED.mkdir(parents=True, exist_ok=True)
    for lang in ("en", "zh_CN"):
        key = "en" if lang == "en" else "zh"
        txt = TXT[key]
        write_text(_STAGED / f"{SVG_BASE}.{lang}.svg",
                   svg_figure(txt["title"], "", txt["lead"], UPDATE_STAGES,
                              txt["legend"], return_from=5,
                              return_caption=txt["return"], lang=key))
        write_text(_STAGED / lang / "index.html",
                   interactive_page(
                       txt["title"], txt["lead"] + txt["page_lead_end"],
                       UPDATE_STAGES, txt["offline"], lang=key))
    (_STAGED / "update_flow.svg").unlink(missing_ok=True)
    (_STAGED / "flow.svg").unlink(missing_ok=True)
    (_STAGED / "index.html").unlink(missing_ok=True)
    return _STAGED

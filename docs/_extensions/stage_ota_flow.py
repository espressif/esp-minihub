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
* ``minihub_vibe_stage_finish`` / the SHA-256 tail trailer /
  ``esp_ota_begin`` / ``esp_ota_end`` — components/minihub_vibe/
* the 800 ms delay before ``esp_restart()`` — minihub_vibe_stage.c (``minihub_vibe_enter``)
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
        "lead": "一次更新六步：读提示 → 后台重探 → 交 URL → 暂存到外部介质 → Vibe Mode 校验并搬运 → 回切 ota_0 重启",
        "return": ("青虚线 = 校验或搬运没过去：Vibe Mode 保留 factory 启动选择，"
                   "下一轮重新搬运暂存的镜像"),
        "legend": ("箭头向右 = 更新的前进方向；青虚线向左 = 重试，回到 Vibe Mode",),
        "page_lead_end": "。卡片自动逐站导览，悬停暂停。",
        "offline": "本页不请求外部资源，离线可读。",
    },
    "en": {
        "title": "MiniHub firmware update",
        "lead": ("Six steps per update: read the hints → re-probe off-task → hand over "
                 "the URL → stage on external media → Vibe Mode verifies and copies → "
                 "select ota_0 and restart"),
        "return": ("Cyan dashed = the verify or the copy did not pass: Vibe Mode keeps "
                   "the factory boot selection and retries the staged image"),
        "legend": ("Arrows right = the direction an update moves; cyan dashed left = "
                   "retry, back to Vibe Mode",),
        "page_lead_end": ". The cards tour themselves and hover pauses the tour.",
        "offline": "This page fetches no external resource and works offline.",
    },
}

UPDATE_STAGES = [
    # (code, label zh, actor en, note zh, note en)
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
    ("minihub_vibe_stage_finish", "暂存到介质", "/vibe/Firmware/firmware.bin",
     "下载流写进外部 LittleFS 卷，运行中的固件不写 flash",
     "The download stream goes to an external LittleFS volume; the running firmware "
     "writes no flash"),
    ("esp_ota_end", "校验并搬运", "SHA-256 + header + checksum",
     "Vibe Mode 比对分区尾部 trailer 的 SHA，不同才用 esp_ota_begin/write/end 写入 ota_0",
     "Vibe Mode compares the SHA in the tail trailer and only copies into ota_0 with "
     "esp_ota_begin/write/end when it differs"),
    ("set_boot_partition", "回切并重启", "800 ms → restart",
     "选中 app,factory 后 800 ms 进 Vibe Mode；搬运完重写 otadata 选中 ota_0 再重启",
     "Selecting app,factory enters Vibe Mode after 800 ms; the copy rewrites otadata "
     "to select ota_0 and restarts"),
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

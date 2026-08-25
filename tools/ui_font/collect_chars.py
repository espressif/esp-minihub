#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Collect characters for minihub_sc.otf from UI JSON, LCD app strings, config, and optional GB2312 charset."""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CHARS_3000_FILE = ROOT / "tools" / "ui_font" / "data" / "chars_3000.txt"


def walk_json(value: object, chars: set[str]) -> None:
    if isinstance(value, dict):
        label_props = value.get("labelProps")
        if isinstance(label_props, dict):
            text = label_props.get("text")
            if isinstance(text, str):
                chars.update(text)
        for key, item in value.items():
            if key in ("text", "label") and isinstance(item, str):
                chars.update(item)
            elif key != "labelProps":
                walk_json(item, chars)
    elif isinstance(value, list):
        for item in value:
            walk_json(item, chars)
    elif isinstance(value, str):
        chars.update(value)


# LCD / login overlay strings that must survive UI-only font builds.
LCD_EXTRA_CHARS = (
    "登录云账号确定邮箱手机号密码图形验证码短信同步退出"
    "请用手机浏览器访问进行网络未连接连接后重试"
    "账邮消在在线能力点适配器概览更新全部商店地图"
    "未已点击拉取云端设备控制器就绪添加用户"
)


def collect_cpp_chars(path: pathlib.Path, chars: set[str]) -> None:
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    chars.update(re.findall(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef·…]", text))
    for match in re.finditer(r'"([^"\n\\]*(?:\\.[^"\n\\]*)*)"', text):
        chars.update(match.group(1))


def load_common_3000() -> str:
    if CHARS_3000_FILE.is_file():
        return CHARS_3000_FILE.read_text(encoding="utf-8").strip()
    # Regenerate if data file missing (e.g. fresh clone before first font build).
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "gen_chars_3000", ROOT / "tools" / "ui_font" / "gen_chars_3000.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("gen_chars_3000.py not found")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.gb2312_level1_chars(3000)


def collect_ui_chars() -> set[str]:
    chars: set[str] = set()

    ui_dir = ROOT / "components" / "minihub_lcd" / "ui"
    for path in ui_dir.rglob("*.json"):
        walk_json(json.loads(path.read_text(encoding="utf-8")), chars)

    cpp = (ROOT / "components" / "minihub_lcd" / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
    chars.update(re.findall(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef·…]", cpp))
    for match in re.finditer(r'"([^"\n\\]*(?:\\.[^"\n\\]*)*)"', cpp):
        chars.update(match.group(1))

    lcd_dir = ROOT / "components" / "minihub_lcd"
    for path in lcd_dir.glob("*.cpp"):
        collect_cpp_chars(path, chars)

    chars.update(LCD_EXTRA_CHARS)

    config_dir = ROOT / "config"
    if config_dir.is_dir():
        for path in config_dir.rglob("*.json"):
            walk_json(json.loads(path.read_text(encoding="utf-8")), chars)

    # Latin, digits, and common UI punctuation used in labels.
    chars.update(" #%'+,-./0123456789:=>?ABCDEFGHIJKLMNOPQRSTUVW[]_abcdefghijklmnopqrstuvwxyz")
    chars.update("·…（）")
    return chars


def collect(*, include_common_3000: bool = False) -> str:
    chars = collect_ui_chars()
    if include_common_3000:
        chars.update(load_common_3000())
    return "".join(sorted(chars))


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect font subset characters for minihub_sc.otf")
    parser.add_argument(
        "--common3000",
        action="store_true",
        help="Include GB2312 level-1 level-1 3000 Hanzi (default for tools/ui_font/build_subset.sh)",
    )
    args = parser.parse_args()
    sys.stdout.write(collect(include_common_3000=args.common3000))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

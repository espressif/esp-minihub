#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
# Build minihub_sc.otf — Source Han Sans subset (UI chars + GB2312 level-1 3000 Hanzi).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP_DIR="${MINIHUB_APP_DIR:-$ROOT/application/minihub}"
SRC_FONT="${SRC_FONT:-$APP_DIR/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf}"
OUT="$ROOT/components/minihub_lcd/ui/fonts/minihub_sc.otf"
PYFT="${PYFT:-pyftsubset}"
# Set MINIHUB_FONT_UI_ONLY=1 for a small UI-only subset (~70 KiB).
COMMON3000="${MINIHUB_FONT_UI_ONLY:-}"

if [[ ! -f "$SRC_FONT" ]]; then
  echo "Source font not found: $SRC_FONT" >&2
  echo "Set SRC_FONT to SourceHanSansSC-Normal.otf" >&2
  exit 1
fi

if [[ -z "$COMMON3000" ]]; then
  python3 "$ROOT/tools/ui_font/gen_chars_3000.py" -o "$ROOT/tools/ui_font/data/chars_3000.txt" >/dev/null
  CHARS="$(python3 "$ROOT/tools/ui_font/collect_chars.py" --common3000)"
  echo "Building 3000-character CJK subset (+ UI ASCII/punctuation)..."
else
  CHARS="$(python3 "$ROOT/tools/ui_font/collect_chars.py")"
  echo "Building UI-only font subset (MINIHUB_FONT_UI_ONLY=1)..."
fi

mkdir -p "$(dirname "$OUT")"
"$PYFT" "$SRC_FONT" \
  --output-file="$OUT" \
  --text="$CHARS" \
  --layout-features='*' \
  --glyph-names --symbol-cmap --legacy-cmap \
  --notdef-glyph --notdef-outline --recommended-glyphs
BYTES="$(wc -c <"$OUT" | tr -d ' ')"
echo "Wrote $OUT ($BYTES bytes, ${#CHARS} unique chars)"
if [[ -z "$COMMON3000" && "$BYTES" -gt 950000 ]]; then
  echo "Note: font is ~1 MiB; use partitions.csv (storage >= 3 MiB). " >&2
fi

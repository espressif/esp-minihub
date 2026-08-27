#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Stage firmware + merged flash image + compiled sdkconfig for CI upload (no Store .mhub).
# Flash images are taken from build/flasher_args.json → flash_files (same set as merge-bin).
#
# Usage: tools/ci/stage_artifacts.sh <app_name>
#
# Env:
#   MINIHUB_CI_ARTIFACTS  — staging root (default: <repo>/ci_artifacts)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="${1:-}"
if [[ -z "$APP" ]]; then
  echo "usage: $0 <app_name>" >&2
  exit 2
fi

APP_DIR="$ROOT/application/$APP"
BUILD="$APP_DIR/build"
ARTIFACTS="${MINIHUB_CI_ARTIFACTS:-$ROOT/ci_artifacts}"
OUT="$ARTIFACTS/$APP"
ARGS_JSON="$BUILD/flasher_args.json"

if [[ ! -d "$BUILD" ]]; then
  echo "ERROR: missing build dir $BUILD" >&2
  exit 1
fi
if [[ ! -f "$ARGS_JSON" ]]; then
  echo "ERROR: missing $ARGS_JSON — run a successful build first" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/firmware"

# Keep IDF-relative layout so flasher_args.json paths still resolve.
copy_rel() {
  local src="$1"
  local rel="${src#"$BUILD"/}"
  local dst="$OUT/firmware/$rel"
  mkdir -p "$(dirname "$dst")"
  cp -a "$src" "$dst"
}

# Same inputs as esptool merge-bin / idf.py flash (portable: no bash mapfile).
FLASH_LIST="$(
  python3 - "$ARGS_JSON" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
files = data.get("flash_files") or {}
for _off, rel in sorted(files.items(), key=lambda kv: int(str(kv[0]), 0)):
    print(rel)
PY
)"

if [[ -z "$FLASH_LIST" ]]; then
  echo "ERROR: no flash_files in $ARGS_JSON" >&2
  exit 1
fi

missing=0
echo "Staging flash_files from flasher_args.json:"
while IFS= read -r rel; do
  [[ -z "$rel" ]] && continue
  src="$BUILD/$rel"
  if [[ ! -f "$src" ]]; then
    echo "  MISSING  $rel" >&2
    missing=1
    continue
  fi
  echo "  $rel"
  copy_rel "$src"
done <<< "$FLASH_LIST"
if [[ "$missing" -ne 0 ]]; then
  echo "ERROR: flasher_args.json flash_files missing on disk" >&2
  exit 1
fi

copy_rel "$ARGS_JSON"

# Effective Kconfig after idf.py build (project root, not in flash_files).
SDKCONFIG="$APP_DIR/sdkconfig"
if [[ -f "$SDKCONFIG" ]]; then
  cp -a "$SDKCONFIG" "$OUT/firmware/sdkconfig"
  echo "  sdkconfig (compiled)"
else
  echo "ERROR: missing compiled sdkconfig $SDKCONFIG — run a successful build first" >&2
  exit 1
fi

# Debug / OTA helpers (not in flash_files)
for f in \
  "$BUILD/${APP}.elf" \
  "$BUILD/${APP}.map" \
  "$BUILD/bootloader/bootloader.elf" \
  "$BUILD/flash_args" \
  "$BUILD/flash_project_args"
do
  if [[ -f "$f" ]]; then
    copy_rel "$f"
  fi
done

# Full-flash merged image (esptool merge-bin @ 0x0)
MERGED="$BUILD/minihub_merged.bin"
if [[ ! -f "$MERGED" ]]; then
  echo "ERROR: missing merged image $MERGED — run build merge first" >&2
  exit 1
fi
cp -a "$MERGED" "$OUT/firmware/${APP}_merged.bin"
cp -a "$MERGED" "$OUT/firmware/minihub_merged.bin"

echo "Staged CI firmware under $OUT"
find "$OUT" -type f | sort | while read -r p; do
  sz="$(wc -c < "$p" | tr -d ' ')"
  echo "  ${p#"$ARTIFACTS/"}  (${sz} bytes)"
done

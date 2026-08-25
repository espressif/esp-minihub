#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Stage firmware + merged flash image for CI upload (no Store .mhub).
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

BUILD="$ROOT/application/$APP/build"
ARTIFACTS="${MINIHUB_CI_ARTIFACTS:-$ROOT/ci_artifacts}"
OUT="$ARTIFACTS/$APP"

if [[ ! -d "$BUILD" ]]; then
  echo "ERROR: missing build dir $BUILD" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/firmware"

# App firmware (partition images + ELF)
for f in \
  "$BUILD/${APP}.bin" \
  "$BUILD/${APP}.elf" \
  "$BUILD/bootloader/bootloader.bin" \
  "$BUILD/partition_table/partition-table.bin" \
  "$BUILD/ota_data_initial.bin" \
  "$BUILD/storage.bin" \
  "$BUILD/modstore.bin" \
  "$BUILD/system.bin" \
  "$BUILD/flasher_args.json"
do
  if [[ -f "$f" ]]; then
    cp -a "$f" "$OUT/firmware/"
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

#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Pack Store .mhub packages when native modules exist after a minihub firmware build.
# No libadapter_*.so is OK (public Host with no plugins) — exit 0 and stage empty dirs.
#
# Env:
#   MINIHUB_STORE_DEPLOY  — deploy root (default: <repo>/ci_artifacts/store)
#   MINIHUB_CI_ARTIFACTS  — staging root (default: <repo>/ci_artifacts)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

ARTIFACTS="${MINIHUB_CI_ARTIFACTS:-$ROOT/ci_artifacts}"
export MINIHUB_STORE_DEPLOY="${MINIHUB_STORE_DEPLOY:-$ARTIFACTS/store}"
export MINIHUB_APP=minihub

OUT="$ARTIFACTS/mhub"
mkdir -p "$MINIHUB_STORE_DEPLOY" "$OUT"

MODULES="$ROOT/application/minihub/build/modules"
shopt -s nullglob
SOS=("$MODULES"/libadapter_*.so)
shopt -u nullglob
if [[ ${#SOS[@]} -eq 0 ]]; then
  echo "No $MODULES/libadapter_*.so — skipping Store pack (no plugins built)."
  echo "CI staged 0 .mhub package(s) under ci_artifacts/mhub/."
  exit 0
fi

echo "Packing Store .mhub from $MODULES → $MINIHUB_STORE_DEPLOY"
./tools/build_firmware.py --app minihub modules all

rm -rf "$OUT"
mkdir -p "$OUT"
if [[ -f "$MINIHUB_STORE_DEPLOY/catalog.json" ]]; then
  cp -a "$MINIHUB_STORE_DEPLOY/catalog.json" "$OUT/"
fi
find "$MINIHUB_STORE_DEPLOY" -mindepth 2 -maxdepth 2 -type f -name '*.mhub' -print0 |
  while IFS= read -r -d '' pkg; do
    mid="$(basename "$(dirname "$pkg")")"
    mkdir -p "$OUT/$mid"
    cp -a "$pkg" "$OUT/$mid/"
  done

echo "Staged Store packages under $OUT"
find "$OUT" -type f | sort | while read -r p; do
  sz="$(wc -c < "$p" | tr -d ' ')"
  echo "  ${p#"$ARTIFACTS/"}  (${sz} bytes)"
done
n="$(find "$OUT" -type f -name '*.mhub' | wc -l | tr -d ' ')"
echo "CI staged $n .mhub package(s) under ci_artifacts/mhub/ (see pack summary above)."

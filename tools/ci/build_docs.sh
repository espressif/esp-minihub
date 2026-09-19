#!/usr/bin/env bash
# CI: build docs into docs/_build/ and stage ci_artifacts/docs/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

bash "$ROOT/tools/build_docs.sh" all

OUT="$ROOT/ci_artifacts/docs"
rm -rf "$OUT"
mkdir -p "$OUT/en" "$OUT/zh_CN"
cp -a "$ROOT/docs/_build/en/esp32s31/html/." "$OUT/en/"
cp -a "$ROOT/docs/_build/zh_CN/esp32s31/html/." "$OUT/zh_CN/"
test -f "$OUT/en/index.html"
test -f "$OUT/zh_CN/index.html"
echo "docs artifacts ready under ci_artifacts/docs/{en,zh_CN}"

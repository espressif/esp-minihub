#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# CI: standalone (no ESP-IDF project) build of every L3 native demo under
# examples/plugins/. Needs cmake and riscv32-esp-elf-gcc on PATH
# (the IDF CI image provides the compiler after export.sh).
#
# Env:
#   MINIHUB_CI_ARTIFACTS — staging root (default: <repo>/ci_artifacts)
#   MINIHUB_PLUGIN_CC    — C compiler (default: riscv32-esp-elf-gcc)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

ARTIFACTS="${MINIHUB_CI_ARTIFACTS:-$ROOT/ci_artifacts}"
OUT="$ARTIFACTS/plugin_standalone"
CC="${MINIHUB_PLUGIN_CC:-riscv32-esp-elf-gcc}"

python3 tools/test_build_plugin_standalone.py -v

command -v cmake >/dev/null
command -v "$CC" >/dev/null
NM="${CC%-gcc}-nm"
if ! command -v "$NM" >/dev/null; then
  NM=nm
fi
command -v "$NM" >/dev/null

IDS=()
while IFS= read -r id; do
  [[ -n "$id" ]] || continue
  IDS+=("$id")
done < <(python3 - <<'PY'
import json
from pathlib import Path

root = Path("examples/plugins")
for d in sorted(p for p in root.iterdir() if p.is_dir()):
    manifest_path = d / "module.json"
    if not manifest_path.is_file() or not (d / "CMakeLists.txt").is_file():
        continue
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("runtime") == "native":
        print(d.name)
PY
)

if [[ ${#IDS[@]} -eq 0 ]]; then
  echo "no L3 native demos under examples/plugins" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

for id in "${IDS[@]}"; do
  echo "== standalone $id =="
  python3 tools/build_plugin_standalone.py "$id" --clean --cc "$CC"
  raw="examples/plugins/$id/build/lib/$id.so"
  mhmd="examples/plugins/$id/build/$id.so"
  python3 tools/check_module_so_layout.py "$raw"
  if ! "$NM" -g --defined-only "$raw" | grep -q 'minihub_module_entry'; then
    echo "error: $raw has no defined minihub_module_entry" >&2
    exit 1
  fi
  python3 - "$mhmd" <<'PY'
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = path.read_bytes()
if len(data) < 16:
    raise SystemExit(f"{path}: too small ({len(data)} bytes)")
magic = struct.unpack_from("<I", data, 0)[0]
if magic != 0x4D484D44:
    raise SystemExit(f"{path}: missing MHMD magic (got 0x{magic:08x})")
if path.stat().st_size < 2048:
    raise SystemExit(f"{path}: suspiciously small ({path.stat().st_size} bytes)")
print(f"OK MHMD {path} ({path.stat().st_size} bytes)")
PY
  cp -a "$mhmd" "$OUT/"
done

echo "standalone OK: ${IDS[*]} → $OUT"

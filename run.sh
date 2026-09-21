#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

pids=()
cleanup() {
  for pid in "${pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

mkdir -p logs

# Website static server only sees MiniHub_website/; expose sibling Store as a subfolder.
if [[ -d MiniHub_store ]]; then
  ln -sfn ../MiniHub_store MiniHub_website/MiniHub_store
fi

$PY -m http.server 62026 --bind 0.0.0.0 --directory MiniHub_website \
  >logs/http_62026.log 2>&1 &
pids+=($!)

$PY store_server.py --deploy MiniHub_store --host 0.0.0.0 --port 62027 \
  >logs/store_62027.log 2>&1 &
pids+=($!)

$PY preview_docs.py --host 0.0.0.0 --port 62028 --build-dir MiniHub_docs \
  >logs/docs_62028.log 2>&1 &
pids+=($!)

sleep 0.5
names=("http.server" "store_server" "preview_docs")
ports=(62026 62027 62028)
logs=(logs/http_62026.log logs/store_62027.log logs/docs_62028.log)
ok=1
for i in 0 1 2; do
  if kill -0 "${pids[$i]}" 2>/dev/null; then
    echo "OK  ${ports[$i]}  ${names[$i]}"
  else
    echo "FAIL ${ports[$i]}  ${names[$i]} — see ${logs[$i]}:"
    sed -n '1,40p' "${logs[$i]}" || true
    ok=0
  fi
done

if [[ "$ok" -ne 1 ]]; then
  exit 1
fi

echo "Ctrl+C to stop all"
wait

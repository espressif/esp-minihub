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

# The static site only sees MiniHub_website/. Point MiniHub_store at the deploy
# root that contains Stores/Common, Firmwares, Assets, and Webs.
DEPLOY_ROOT="${MINIHUB_DEPLOY_DIR:-}"
if [[ -z "$DEPLOY_ROOT" && -d MiniHub_store ]]; then
  DEPLOY_ROOT="MiniHub_store"
fi
if [[ -z "$DEPLOY_ROOT" ]]; then
  # Fall back to the default the tools use (MINIHUB_PLUGINS_DEPLOY_DIR, else
  # ~/workspace/minihub_store). Without this a bare ./run.sh served a
  # non-existent "MiniHub_store": empty /api/catalog, /api/firmware, /api/web.
  DEPLOY_ROOT="${MINIHUB_PLUGINS_DEPLOY_DIR:-$HOME/workspace/minihub_store}"
fi
if [[ ! -d "$DEPLOY_ROOT" ]]; then
  echo "run.sh: deploy root not found: $DEPLOY_ROOT (set MINIHUB_DEPLOY_DIR)" >&2
  exit 1
fi
if [[ -n "$DEPLOY_ROOT" && -d "$DEPLOY_ROOT" && -d MiniHub_website ]]; then
  ln -sfn "$(python3 -c 'import os,sys; print(os.path.relpath(os.path.abspath(sys.argv[1]), "MiniHub_website"))' "$DEPLOY_ROOT")" \
    MiniHub_website/MiniHub_store
fi

$PY -m http.server 62026 --bind 0.0.0.0 --directory MiniHub_website \
  >logs/http_62026.log 2>&1 &
pids+=($!)

$PY tools/store_server.py --deploy "${DEPLOY_ROOT:-MiniHub_store}" --host 0.0.0.0 --port 62027 \
  >logs/store_62027.log 2>&1 &
pids+=($!)

$PY tools/preview_docs.py --host 0.0.0.0 --port 62028 --build-dir docs/_build \
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
    # Docs preview depends on a separate build (tools/build_docs.sh); do not
    # fail the whole deploy when it is merely not built yet.
    if [[ "${names[$i]}" == "preview_docs" ]]; then
      echo "WARN ${ports[$i]}  ${names[$i]} (docs not built? run tools/build_docs.sh) — see ${logs[$i]}:"
      sed -n '1,40p' "${logs[$i]}" || true
    else
      echo "FAIL ${ports[$i]}  ${names[$i]} — see ${logs[$i]}:"
      sed -n '1,40p' "${logs[$i]}" || true
      ok=0
    fi
  fi
done

if [[ "$ok" -ne 1 ]]; then
  exit 1
fi

echo "Ctrl+C to stop all"
wait

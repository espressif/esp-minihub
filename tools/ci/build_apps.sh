#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Build every application/<name>/ firmware (+ merge). Does not pack Store .mhub —
# that is tools/ci/pack_store_modules.sh (one shared CI job).
#
# Env:
#   MINIHUB_APP — product = application/<name>/ (default: all apps under application/)
#   MINIHUB_CI_ARTIFACTS  — staging root (default: <repo>/ci_artifacts)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

export MINIHUB_APP="${MINIHUB_APP:-}"
ARTIFACTS="${MINIHUB_CI_ARTIFACTS:-$ROOT/ci_artifacts}"
# Firmware jobs never pack .mhub (single pack:mhub job owns Store packages)
export MINIHUB_SKIP_MHUB=1

if [[ -n "${MINIHUB_APP:-}" ]]; then
  APPS="$MINIHUB_APP"
else
  APPS="$(python3 "$ROOT/tools/ci/list_apps.py")"
fi

if [[ -z "${APPS}" ]]; then
  echo "No applications to build" >&2
  exit 1
fi

mkdir -p "$ARTIFACTS"

echo "Artifacts=${ARTIFACTS}"
echo "Apps:"
echo "$APPS"

while IFS= read -r app; do
  [[ -z "$app" ]] && continue
  echo "======== build + merge application/${app} ========"
  # Per-app submodule init only (do not init the other app's deps).
  case "$app" in
    claw_minihub)
      git submodule update --init --depth 1 application/claw_minihub/components/esp-claw
      ;;
    minihub)
      # RainMaker may be enabled by defaults / MINIHUB_BUILD_RAINMAKER
      if [[ "${MINIHUB_BUILD_RAINMAKER:-}" != "0" ]]; then
        git submodule update --init --depth 1 application/minihub/components/esp-rainmaker
      fi
      ;;
  esac
  ./tools/build_firmware.py --app "$app" build merge
  # Ensure modules/ exists so CI artifact paths are always valid (minihub fills it)
  mkdir -p "application/${app}/build/modules"
  bash "$ROOT/tools/ci/stage_artifacts.sh" "$app"
done <<< "$APPS"

echo "All application firmware builds finished."

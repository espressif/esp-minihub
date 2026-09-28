#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Apply an optional HW recipe under application/<app>/configs/sdkconfig/variants/
# onto app.yml (product is still MINIHUB_APP / application folder name).
#
# Usage:
#   ./tools/apply_sdkconfig_variant.sh lcd          # Wi-Fi + LCD recipe
#   ./tools/apply_sdkconfig_variant.sh --restore    # back to default app.yml from git
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_NAME="${MINIHUB_APP:-minihub}"
APP_SDK="$ROOT/application/$APP_NAME/configs/sdkconfig"

if [[ "${1:-}" == "--restore" ]]; then
  SRC="$APP_SDK/variants/default.yml"
  if [[ ! -f "$SRC" ]]; then
    echo "missing $SRC" >&2
    exit 1
  fi
  cp "$SRC" "$APP_SDK/app.yml"
  echo "Restored application/$APP_NAME/configs/sdkconfig/app.yml from variants/default.yml"
  exit 0
fi

VARIANT="${1:-}"
if [[ -z "$VARIANT" ]]; then
  echo "usage: $0 <variant>|--restore" >&2
  echo "variants:" >&2
  ls "$APP_SDK/variants"/*.yml 2>/dev/null | xargs -n1 basename | sed 's/\.yml$//' >&2 || true
  exit 2
fi

SRC="$APP_SDK/variants/${VARIANT}.yml"
DST="$APP_SDK/app.yml"
if [[ ! -f "$SRC" ]]; then
  echo "missing $SRC" >&2
  exit 1
fi
cp "$SRC" "$DST"
echo "Applied variant '$VARIANT' → $DST"
echo "Product remains: $APP_NAME (MINIHUB_APP)"
echo "Next: ./tools/build_firmware.py -app $APP_NAME build"

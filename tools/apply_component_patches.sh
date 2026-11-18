#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Re-apply local patches under patches/ after component re-download or
# resetting a sibling tree (e.g. ../esp-rainmaker).
#
# Patch headers may include:
#   Apply-Root: <path-relative-to-MiniHub>   # default: application/<app>
#
# Usage:
#   ./tools/apply_component_patches.sh
#   ./tools/apply_component_patches.sh --check   # dry-run, exit 1 if patch needed
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$ROOT/patches"
CHECK=0
# Prefer application project managed_components (monorepo layout).
APP_DIR="${MINIHUB_APP_DIR:-$ROOT/application/minihub}"

if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

patch_apply_root() {
  local rel
  rel="$(grep -E '^Apply-Root:' "$1" | head -1 | sed 's/^Apply-Root:[[:space:]]*//' || true)"
  if [[ -z "$rel" ]]; then
    if [[ -d "$APP_DIR/managed_components" ]]; then
      echo "$APP_DIR"
    else
      echo "$ROOT"
    fi
  else
    # Rainmaker (and other Component Manager deps) live under each product's
    # managed_components/. Older patches hard-code application/minihub/...;
    # remap that prefix onto the app currently being built (MINIHUB_APP_DIR).
    if [[ "$rel" == application/minihub/* ]]; then
      rel="$APP_DIR/${rel#application/minihub/}"
      # Strip the MiniHub root prefix if APP_DIR was absolute.
      rel="${rel#"$ROOT"/}"
    fi
    # Resolve relative to MiniHub (supports ../esp-matter)
    (cd "$ROOT" && cd "$rel" && pwd)
  fi
}

patch_target() {
  grep -E '^--- a/' "$1" | head -1 | sed 's|^--- a/||'
}

patch_marker() {
  # Prefer a distinctive added line (not a lone #if) for idempotency checks.
  local m
  m="$(grep -E '^\+.*force software crypto' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "$m"
    return
  fi
  # Match the distinctive phrase only — log level (LOGI/LOGW) may differ in a
  # previously hand-edited managed_components tree without changing the patch.
  m="$(grep -E '^\+.*Deferring Challenge Response init' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "Deferring Challenge Response init until self claiming completes"
    return
  fi
  m="$(grep -E '^\+.*Client certificate not provisioned yet' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "Client certificate not provisioned yet; waiting for claim"
    return
  fi
  m="$(grep -E '^\+.*Peer not found for CHIPoBLE write' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "$m"
    return
  fi
  m="$(grep -E '^\+.*minihub_psram_text_exec' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "$m"
    return
  fi
  m="$(grep -E '^\+.*value-init EspOtaRequestorEntry' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "$m"
    return
  fi
  m="$(grep -E '^\+.*CHIP UpperCamelCase rename' "$1" | head -1 | sed 's/^+//' || true)"
  if [[ -n "$m" ]]; then
    echo "$m"
    return
  fi
  grep -E '^\+[^+]' "$1" | head -1 | sed 's/^+//'
}

is_applied() {
  local target="$1"
  local marker="$2"
  grep -qF "$marker" "$target"
}

apply_one() {
  local patch_file="$1"
  local apply_root rel target marker
  apply_root="$(patch_apply_root "$patch_file")"
  rel="$(patch_target "$patch_file")"
  if [[ -z "$rel" ]]; then
    echo "skip (no path): $(basename "$patch_file")" >&2
    return 0
  fi
  target="$apply_root/$rel"
  if [[ ! -f "$target" ]]; then
    # Component may be absent for this profile (e.g. esp-boost only with LCD/GUI).
    # Do not fail the whole build — patch applies once the component is present.
    echo "skip (missing target): $(basename "$patch_file") → $target" >&2
    return 0
  fi
  marker="$(patch_marker "$patch_file")"
  if [[ -z "$marker" ]]; then
    echo "skip (no marker line): $(basename "$patch_file")" >&2
    return 1
  fi
  if is_applied "$target" "$marker"; then
    echo "already applied: $(basename "$patch_file")"
    return 0
  fi
  if [[ "$CHECK" -eq 1 ]]; then
    echo "needs apply: $(basename "$patch_file")"
    return 2
  fi
  echo "applying: $(basename "$patch_file") (root=$apply_root)"
  patch -p1 -N -d "$apply_root" -i "$patch_file"
}

status=0
shopt -s nullglob

for patch_file in "$PATCH_DIR"/*.patch; do
  apply_one "$patch_file" || status=$?
done

if [[ "$CHECK" -eq 1 && "$status" -eq 0 ]]; then
  echo "all managed patches applied"
fi

exit "$status"

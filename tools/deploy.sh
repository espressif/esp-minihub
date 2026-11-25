#!/usr/bin/env bash
#
# One-click deploy for MiniHub.
#
# Regenerates every served artifact under the deploy root (device-type
# registry, plugin catalogs, firmware), then starts the local servers
# (static site + store server + docs preview) via run.sh.
#
# Deploy root precedence (mirrors store_paths.resolve_deploy_dir):
#   $MINIHUB_DEPLOY_DIR  >  ./MiniHub_store  >  ~/workspace/minihub_store
#
# Plugin packing: step [4/5] runs tools/pack_store_module.py pack all
# (scanning examples/plugins and MINIHUB_PLUGIN_DIRS) before re-indexing.
# pack all skips native and hybrid modules.
#
# Firmware publish copies the *already built* application image
# (application/<app>/build/<app>.bin) into Firmwares/<chip>/<version>/ with a
# release_notes.txt and the full flash set (bootloader, partitions, merged.bin,
# <app>.elf, flash_command.txt). The build itself is left to build_firmware.py -- this step
# only publishes. When the bin is absent it warns and skips (no failure), same
# as the plugin flow. GET /api/firmware derives the catalog by walking the
# Firmwares tree at request time, so no index file is written here. The
# <version> folder defaults to CONFIG_MINIHUB_VERSION from the built sdkconfig
# (else git describe), matching what the device reports as firmware_version.
# The web bundle version (Webs/<vX>/ and window.mhWebVersion) has its OWN Kconfig,
# CONFIG_MINIHUB_WEB_VERSION, because web and firmware releases are usually not in
# sync -- a web-only fix bumps the web version without touching the firmware.
# It is also what the Host build stamps into the storage image, so published and
# freshly flashed bundles always report the same version.
# LCD Brookesia UI (LCD_UI/<vX>/ → /data/ui) uses CONFIG_MINIHUB_LCD_UI_VERSION the
# same way; panel-only fixes bump it without a firmware or web release.
# All three are read from the built sdkconfig; see the "Release versions" note
# right after the argument parsing.
#
# Run from anywhere: this script resolves the repo root as the parent of its
# own directory (tools/) so the tools/..., components/..., and run.sh references
# below stay valid.
#
# Options:
#   --app <name>         product folder under application/ (default $MINIHUB_APP or minihub)
#   --fw-version <ver>   Firmwares/<chip>/<ver>/ (default: CONFIG_MINIHUB_VERSION, else git describe)
#   --fw-chip <chip>     chip folder (default ESP32S31)
#   --fw-note <file>     file copied to Firmwares/<chip>/<version>/release_notes.txt
#   --web-version <ver>  Webs/<ver>/ (default: v + CONFIG_MINIHUB_WEB_VERSION, else v + git describe)
#   --ui-version <ver>   LCD_UI/<ver>/ (default: v + CONFIG_MINIHUB_LCD_UI_VERSION, else v + git describe)
#   --no-firmware        skip the firmware publish step
#   -h | --help          show this help
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

# --- argument defaults -------------------------------------------------------
APP="${MINIHUB_APP:-minihub}"
FW_CHIP="ESP32S31"
FW_VERSION=""
FW_NOTE=""
SKIP_FW=0
WEB_VERSION=""
UI_VERSION=""

usage() {
  grep -E '^#' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app)
      APP="$2"; shift 2;;
    --fw-version)
      FW_VERSION="$2"; shift 2;;
    --fw-chip)
      FW_CHIP="$2"; shift 2;;
    --fw-note)
      FW_NOTE="$2"; shift 2;;
    --web-version)
      WEB_VERSION="$2"; shift 2;;
    --ui-version)
      UI_VERSION="$2"; shift 2;;
    --no-firmware)
      SKIP_FW=1; shift;;
    -h|--help)
      usage 0;;
    *)
      echo "deploy.sh: unknown argument: $1" >&2
      usage 2;;
  esac
done

FW_CHIP="$(printf '%s' "$FW_CHIP" | tr '[:upper:]' '[:lower:]')"

# Release versions come from independent Kconfig values -- firmware, web, and
# LCD UI releases are usually not in sync, so a single-channel fix bumps only
# that channel. Manage them under `idf.py menuconfig` > MiniHub:
#   CONFIG_MINIHUB_VERSION         firmware folder / reported firmware_version
#                                  (<ver>, no leading "v")
#   CONFIG_MINIHUB_WEB_VERSION     web bundle -- Webs/v<ver>/ + version.txt +
#                                  window.mhWebVersion (<ver>, no "v")
#   CONFIG_MINIHUB_LCD_UI_VERSION  LCD UI -- LCD_UI/v<ver>/ + /data/ui/version.txt
# All default to MINIHUB_VERSION; override per-run with --fw-version /
# --web-version / --ui-version.
sdkconfig_value() {  # $1 = sdkconfig path, $2 = config key
  local sdk="$1" key="$2" v=""
  if [[ -f "$sdk" ]]; then
    # `|| true`: a missing key makes grep return 1, which `set -o pipefail` would
    # turn into a hard failure of the whole script under `set -e`.
    v="$(grep -E "^${key}=" "$sdk" | head -n1 \
      | sed -E 's/^[^=]*="?([^"]*)"?$/\1/' || true)"
  fi
  if [[ -z "$v" ]]; then
    v="$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo 0.1.0)"
  fi
  echo "$v"
}

if [[ -z "$FW_VERSION" || -z "$WEB_VERSION" || -z "$UI_VERSION" ]]; then
  SDKCONFIG="$ROOT/application/$APP/build/sdkconfig"
  if [[ ! -f "$SDKCONFIG" ]]; then
    SDKCONFIG="$ROOT/application/$APP/sdkconfig"
  fi
  if [[ -z "$FW_VERSION" ]]; then
    FW_VERSION="$(sdkconfig_value "$SDKCONFIG" CONFIG_MINIHUB_VERSION)"
  fi
  if [[ -z "$WEB_VERSION" ]]; then
    WEB_VERSION="v$(printf '%s' "$(sdkconfig_value "$SDKCONFIG" CONFIG_MINIHUB_WEB_VERSION)" \
      | sed -E 's/^v//')"
  fi
  if [[ -z "$UI_VERSION" ]]; then
    UI_VERSION="v$(printf '%s' "$(sdkconfig_value "$SDKCONFIG" CONFIG_MINIHUB_LCD_UI_VERSION)" \
      | sed -E 's/^v//')"
  fi
fi

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

if [[ -n "${MINIHUB_DEPLOY_DIR:-}" ]]; then
  DEPLOY_ROOT="${MINIHUB_DEPLOY_DIR}"
elif [[ -d MiniHub_store ]]; then
  DEPLOY_ROOT="MiniHub_store"
else
  DEPLOY_ROOT="${HOME}/workspace/minihub_store"
fi

mkdir -p "$DEPLOY_ROOT"
export MINIHUB_DEPLOY_DIR="$DEPLOY_ROOT"

echo "==> deploy root: $DEPLOY_ROOT"

# 1) Web assets: minify components/minihub_web/web -> Webs/<WEB_VERSION>/
echo "==> [1/5] web assets (Webs/$WEB_VERSION)"
_WEB_NOTES="components/minihub_web/web/release_notes.txt"
_MINIFY_NOTES_FLAG=""
if [ -f "$_WEB_NOTES" ]; then
  _MINIFY_NOTES_FLAG="--notes $_WEB_NOTES"
fi
# Publish plain/ and gzip/ parallel trees (not sibling .gz in one folder) so
# either encoding can be uploaded alone; Hubs on any WEB_ASSET_* mode can OTA.
$PY tools/minify_web_assets.py --src components/minihub_web/web \
  --deploy-root "$DEPLOY_ROOT" --web-version "$WEB_VERSION" \
  --encoding both \
  $_MINIFY_NOTES_FLAG

# 2) LCD UI: minify components/minihub_lcd/ui -> LCD_UI/<UI_VERSION>/
echo "==> [2/5] LCD UI (LCD_UI/$UI_VERSION)"
_UI_NOTES="components/minihub_lcd/ui/release_notes.txt"
_UI_NOTES_FLAG=""
if [ -f "$_UI_NOTES" ]; then
  _UI_NOTES_FLAG="--notes $_UI_NOTES"
fi
if [ -f components/minihub_lcd/ui/root.json ]; then
  $PY tools/minify_lcd_ui_assets.py --src components/minihub_lcd/ui \
    --deploy-root "$DEPLOY_ROOT" --ui-version "$UI_VERSION" \
    $_UI_NOTES_FLAG
else
  echo "==> skip LCD UI: no components/minihub_lcd/ui/root.json"
fi

# 3) Device-type registry. Web / UI / asset manifests are served dynamically by
#    store_server.py (GET /api/web, /api/ui, /api/assets) — no static index.json.
echo "==> [3/5] device-type registry"
$PY tools/gen_deploy_index.py --deploy "$DEPLOY_ROOT" --device-widget components/minihub_device_widget --sync-source

# 4) Plugin pack (+ abi blocks) then catalogs.
#    JS and config-only modules only. Native and hybrid modules are omitted.
echo "==> [4/5] plugin catalogs"
$PY tools/pack_store_module.py pack all --continue-on-error --deploy "$DEPLOY_ROOT" || true
$PY tools/sync_store_catalog.py --from-store --deploy "$DEPLOY_ROOT"

# 5) Firmware publish.
#    One folder: Firmwares/<chip>/<version>/
#      <app>.bin + release_notes.txt     OTA image (GET /api/firmware)
#      bootloader, partition-table, ota_data, storage, modstore, merged.bin,
#      <app>.elf, flash_command.txt      full flash set for a flashing tool
echo "==> [5/5] firmware publish"
if [[ "$SKIP_FW" -eq 1 ]]; then
  echo "==> skipped (--no-firmware)"
else
  FW_BIN="$ROOT/application/$APP/build/$APP.bin"
  FW_DEST_DIR="$DEPLOY_ROOT/Firmwares/$FW_CHIP/$FW_VERSION"
  if [[ ! -f "$FW_BIN" ]]; then
    echo "==> skip firmware: no built image at $FW_BIN (run build_firmware.py first)"
  else
    mkdir -p "$FW_DEST_DIR"
    cp -f "$FW_BIN" "$FW_DEST_DIR/$APP.bin"
    if [[ -n "$FW_NOTE" && -f "$FW_NOTE" ]]; then
      cp -f "$FW_NOTE" "$FW_DEST_DIR/release_notes.txt"
    elif [[ -f "$ROOT/application/$APP/build/release_notes.txt" ]]; then
      cp -f "$ROOT/application/$APP/build/release_notes.txt" "$FW_DEST_DIR/release_notes.txt"
    else
      printf 'Firmware %s for %s\n' "$FW_VERSION" "$FW_CHIP" > "$FW_DEST_DIR/release_notes.txt"
    fi
    echo "==> firmware: $FW_DEST_DIR"
    $PY tools/gen_flash_binaries.py --deploy "$DEPLOY_ROOT" --chip "$FW_CHIP" --version "$FW_VERSION" --app "$APP" || \
      echo "==> warn: flash image generation incomplete (esptool unavailable or build missing?)"
  fi
fi

echo "==> artifacts ready; starting servers (Ctrl+C to stop)"
exec bash run.sh

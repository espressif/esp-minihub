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
# Plugin packing: step [3/4] runs tools/pack_store_module.py pack all
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
# Both are read from the built sdkconfig; see the "Release versions" note right
# after the argument parsing.
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

# Release versions come from two independent Kconfig values -- web and firmware
# releases are usually not in sync, so a web-only fix bumps only the web one.
# Manage both under `idf.py menuconfig` > MiniHub:
#   CONFIG_MINIHUB_VERSION      firmware folder / reported firmware_version
#                               (<ver>, no leading "v")
#   CONFIG_MINIHUB_WEB_VERSION  web bundle version -- single source of truth.
#                               Webs/v<ver>/ folder name (the leading "v" is UI
#                               style) and the value stamped into the bundle as
#                               version.txt + window.mhWebVersion (<ver>, no "v")
# Both default to MINIHUB_VERSION (which also drives the product/sidebar
# version); each can still be overridden per-run with --fw-version / --web-version.
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

if [[ -z "$FW_VERSION" || -z "$WEB_VERSION" ]]; then
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
echo "==> [1/4] web assets (Webs/$WEB_VERSION)"
_WEB_NOTES="components/minihub_web/web/release_notes.txt"
_MINIFY_NOTES_FLAG=""
if [ -f "$_WEB_NOTES" ]; then
  _MINIFY_NOTES_FLAG="--notes $_WEB_NOTES"
fi
$PY tools/minify_web_assets.py --src components/minihub_web/web \
  --deploy-root "$DEPLOY_ROOT" --web-version "$WEB_VERSION" \
  $_MINIFY_NOTES_FLAG

# 2) Device-type registry. Web and asset manifests are no longer written as
#    static index.json files: store_server.py serves GET /api/web and
#    GET /api/assets by walking Webs/<version>/ and Assets/ at request time.
echo "==> [2/4] device-type registry"
$PY tools/gen_deploy_index.py --deploy "$DEPLOY_ROOT" --device-widget components/minihub_device_widget --sync-source

# 3) Plugin pack (+ abi blocks) then catalogs.
#    JS and config-only modules only. Native and hybrid modules are omitted.
echo "==> [3/4] plugin catalogs"
$PY tools/pack_store_module.py pack all --continue-on-error --deploy "$DEPLOY_ROOT" || true
$PY tools/sync_store_catalog.py --from-store --deploy "$DEPLOY_ROOT"

# 4) Firmware publish.
#    One folder: Firmwares/<chip>/<version>/
#      <app>.bin + release_notes.txt     OTA image (GET /api/firmware)
#      bootloader, partition-table, ota_data, storage, modstore, merged.bin,
#      <app>.elf, flash_command.txt      full flash set for a flashing tool
echo "==> [4/4] firmware publish"
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

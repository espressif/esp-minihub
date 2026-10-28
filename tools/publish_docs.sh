#!/usr/bin/env bash
# Publish a built MiniHub docs version into a versioned tree (self-hosted).
# Usage: tools/publish_docs.sh <version> [publish_root]
#   Copies docs/_build/<lang>/html into <publish_root>/<lang>/<version>/
#   and drops the shared version switcher (docs/_static/minihub_versions.js) into each copy.
#
# Build the guide first (per language with ./tools/build_docs.sh), then run this to
# place the HTML under the layout sphinx_idf_theme's version switcher expects
# (<lang>/<version>/<page>.html). MiniHub ships a single target (ESP32-S31), so the
# final path has NO <target> segment. To add another version later, rebuild with
# MINIHUB_DOCS_VERSION=<ver>, run this again, and add that version's entry to
# docs/_static/minihub_versions.js (see its header). Re-running for an existing
# version refreshes its files; the switcher file is shared across all versions.
set -euo pipefail

VERSION="${1:?Usage: tools/publish_docs.sh <version> [publish_root]}"
PUBLISH_ROOT="${2:-docs/_build/publish}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="$ROOT/docs"

for lang in en zh_CN; do
  src="$DOCS/_build/$lang/html"
  if [[ ! -d "$src" ]]; then
    echo "missing built docs: $src (run ./tools/build_docs.sh $lang first)" >&2
    exit 1
  fi
  dst="$PUBLISH_ROOT/$lang/$VERSION"
  rm -rf "$dst"
  mkdir -p "$dst"
  cp -R "$src"/. "$dst"/
  # Copy the shared version switcher into this version's _static.
  mkdir -p "$dst/_static"
  cp "$DOCS/_static/minihub_versions.js" "$dst/_static/"
done

echo "Published version '$VERSION' to $PUBLISH_ROOT/{en,zh_CN}/$VERSION/"
echo "Remember to add '$VERSION' to docs/_static/minihub_versions.js VERSIONS and"
echo "re-run this script for every version so each copy lists all versions."

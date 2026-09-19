#!/usr/bin/env bash
# Build MiniHub Programming Guide (esp-docs). Output: docs/_build/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="$ROOT/docs"
VENV_DOCS="$ROOT/.venv-docs"

ensure_build_docs() {
  if [[ -x "$VENV_DOCS/bin/build-docs" ]]; then
    # shellcheck disable=SC1091
    source "$VENV_DOCS/bin/activate"
    return 0
  fi
  if command -v build-docs >/dev/null 2>&1; then
    return 0
  fi
  echo "build-docs not found. Install esp-docs once:" >&2
  echo "  python3 -m venv $VENV_DOCS" >&2
  echo "  source $VENV_DOCS/bin/activate" >&2
  echo "  pip install -r docs/requirements.txt" >&2
  echo "Then re-run ./tools/build_docs.sh" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage: tools/build_docs.sh [en|zh_CN|all]

Build HTML under docs/_build/ (always; never repo-root _build/).

  en     English only
  zh_CN  Chinese only
  all    both languages (default)
EOF
}

check_rst_parity() {
  comm -3 \
    <(cd "$DOCS/en" && find . -name '*.rst' | sort) \
    <(cd "$DOCS/zh_CN" && find . -name '*.rst' | sort) \
    | tee /tmp/minihub_docs_rst_diff.txt
  if [[ -s /tmp/minihub_docs_rst_diff.txt ]]; then
    echo "docs/en and docs/zh_CN .rst trees differ" >&2
    exit 1
  fi
}

# Homebrew doxygen 1.18 on macOS SIGBUS'es if the Sphinx build dir still has
# leftover xml/doctrees from a previous crash (mmap of truncated XML).
clean_lang_build() {
  local lang="$1"
  rm -rf "$DOCS/xml" "$DOCS/xml_in" "$DOCS/man" \
    "$DOCS/_build/$lang"
}

build_lang() {
  local lang="$1"
  local attempt
  echo "==> build-docs -bs html -t esp32s31 -l $lang (cwd: docs/)"
  for attempt in 1 2; do
    clean_lang_build "$lang"
    if (cd "$DOCS" && build-docs -bs html -t esp32s31 -l "$lang"); then
      return 0
    fi
    local log="$DOCS/_build/$lang/esp32s31/sphinx-warning-log.txt"
    if [[ "$attempt" -eq 1 && -f "$log" ]] && grep -q 'SIGBUS\|died with' "$log"; then
      echo "doxygen crashed (SIGBUS); wiping _build/$lang and retrying once..." >&2
      continue
    fi
    return 1
  done
  return 1
}

assert_catalog_packed() {
  local html="$1"
  local missing=0
  local rel
  for rel in \
    _static/catalog.zip \
    _static/catalog/model/devices.csv \
    _static/catalog/model/parameters.csv \
    _static/catalog/model/ui_elements.csv \
    _static/catalog/model/services.csv \
    _static/catalog/images/devices/light.svg \
    _static/catalog/images/devices/switch.svg \
    _static/catalog/examples/color_bulb/devices.json \
    _static/catalog/examples/meter_plug/devices.json
  do
    if [[ ! -f "$html/$rel" ]]; then
      echo "docs HTML missing packaged catalog file: $html/$rel" >&2
      missing=1
    fi
  done
  return "$missing"
}

case "${1:-all}" in
  -h|--help|help) usage; exit 0 ;;
  en) ensure_build_docs; check_rst_parity; build_lang en; assert_catalog_packed "$DOCS/_build/en/esp32s31/html" ;;
  zh_CN) ensure_build_docs; check_rst_parity; build_lang zh_CN; assert_catalog_packed "$DOCS/_build/zh_CN/esp32s31/html" ;;
  all)
    ensure_build_docs
    check_rst_parity
    build_lang en
    assert_catalog_packed "$DOCS/_build/en/esp32s31/html"
    build_lang zh_CN
    assert_catalog_packed "$DOCS/_build/zh_CN/esp32s31/html"
    ;;
  *) echo "Unknown language: $1" >&2; usage >&2; exit 1 ;;
esac

echo "HTML: $DOCS/_build/{en,zh_CN}/esp32s31/html/"

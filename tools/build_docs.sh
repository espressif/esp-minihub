#!/usr/bin/env bash
# Build MiniHub Programming Guide (esp-docs). Output: docs/_build/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="$ROOT/docs"
VENV_DOCS="$ROOT/.venv-docs"
# Build target (SoC). MiniHub docs are target-agnostic (a single product with no
# per-chip content), so we build with the neutral "generic" target. The theme
# then renders a version switcher only — no target switcher, no <target> segment
# in the URL. The final HTML is relocated to _build/<lang>/html (see build_lang).
TARGET="generic"

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
  echo "==> build-docs -bs html -t $TARGET -l $lang (cwd: docs/)"
  for attempt in 1 2; do
    clean_lang_build "$lang"
    # esp-docs prefixes every log line with "<lang>/<target>:" (target defaults
    # to "generic" when -t is omitted). That label is cosmetic (the real path is
    # relocated away below), so strip it for a clean log. pipefail (set above)
    # still propagates build-docs' real exit code through the sed.
    if (cd "$DOCS" && build-docs -bs html -l "$lang" 2>&1 \
          | sed -E "s#^${lang}/${TARGET}:[[:space:]]*##"); then
      # esp-docs always writes to _build/<lang>/<target>/html. Relocate so the
      # final layout is _build/<lang>/html (no target segment), matching the
      # deployed docs structure. If a future esp-docs writes directly to
      # _build/<lang>/html, the relocation below is simply skipped.
      local built="$DOCS/_build/$lang/$TARGET"
      if [[ -d "$built/html" ]]; then
        rm -rf "$DOCS/_build/$lang/html"
        mv "$built/html" "$DOCS/_build/$lang/html"
        rm -rf "$built"
      fi
      drop_unreferenced_theme_font "$DOCS/_build/$lang/html"
      return 0
    fi
    local log="$DOCS/_build/$lang/$TARGET/sphinx-warning-log.txt"
    if [[ "$attempt" -eq 1 && -f "$log" ]] && grep -q 'SIGBUS\|died with' "$log"; then
      echo "doxygen crashed (SIGBUS); wiping _build/$lang and retrying once..." >&2
      continue
    fi
    return 1
  done
  return 1
}

drop_unreferenced_theme_font() {
  # esp_docs copies NotoSansSC-Regular.otf (8.3 MB, more than half the built tree)
  # into every _static, and nothing in this guide or its theme references it: the
  # CJK text renders with the viewer's own font. Remove it only while that stays
  # true, so a future theme that does use it is never silently broken.
  local html="$1" font="$1/_static/NotoSansSC-Regular.otf"
  [[ -f "$font" ]] || return 0
  if grep -rl "NotoSansSC" --include="*.css" --include="*.html" --include="*.js" "$html" \
       >/dev/null 2>&1; then
    echo "keeping _static/NotoSansSC-Regular.otf: the build references it"
    return 0
  fi
  rm -f "$font"
  echo "dropped unreferenced theme font: _static/NotoSansSC-Regular.otf (8.3 MB)"
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
    _static/catalog/images/devices/switch.svg
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
  en) ensure_build_docs; check_rst_parity; build_lang en; assert_catalog_packed "$DOCS/_build/en/html" ;;
  zh_CN) ensure_build_docs; check_rst_parity; build_lang zh_CN; assert_catalog_packed "$DOCS/_build/zh_CN/html" ;;
  all)
    ensure_build_docs
    check_rst_parity
    build_lang en
    assert_catalog_packed "$DOCS/_build/en/html"
    build_lang zh_CN
    assert_catalog_packed "$DOCS/_build/zh_CN/html"
    ;;
  *) echo "Unknown language: $1" >&2; usage >&2; exit 1 ;;
esac

echo "HTML: $DOCS/_build/{en,zh_CN}/html/"

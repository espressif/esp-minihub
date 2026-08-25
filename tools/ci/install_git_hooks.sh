#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Install local git hooks: pre-commit + pre-push (+ commit-msg strip Cursor).
# Run once per clone:  bash tools/ci/install_git_hooks.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if ! command -v pre-commit >/dev/null 2>&1; then
  python3 -m pip install --user pre-commit
fi

pre-commit install -t pre-commit -t pre-push -t commit-msg

# Optional: strip Cursor co-author trailers from commit messages
STRIP="$ROOT/tools/git-hooks/commit-msg-strip-cursor"
HOOK="$ROOT/.git/hooks/commit-msg"
if [[ -x "$STRIP" ]]; then
  if [[ -f "$HOOK" ]] && grep -q 'commit-msg-strip-cursor' "$HOOK" 2>/dev/null; then
    :
  elif [[ -f "$HOOK" ]]; then
    {
      echo ""
      echo "# MiniHub: strip Cursor attribution"
      echo "\"$STRIP\" \"\$1\""
    } >> "$HOOK"
  else
    {
      echo "#!/usr/bin/env bash"
      echo "\"$STRIP\" \"\$1\""
    } > "$HOOK"
    chmod +x "$HOOK"
  fi
fi

echo "Installed pre-commit hooks (commit + push). git push will run checks."

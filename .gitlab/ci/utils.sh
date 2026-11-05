#!/usr/bin/env bash
# CI helpers shared by .gitlab/ci/*.yml. Sourced from before_script, so the
# functions and exports stay available to script.
#
# add_ssh_keys / add_doc_server_ssh_keys follow espressif/esp-at
# .gitlab/ci/utils.sh (itself copied from esp-idf tools/ci/utils.sh).

function add_ssh_keys() {
  local key_string="${1}"
  mkdir -p ~/.ssh
  chmod 700 ~/.ssh
  # esp-at writes the base64 to a file and decodes it with GNU
  # `base64 --decode --ignore-garbage`. Keep the pipe + `base64 -d` form instead:
  # it is the same on the Debian CI image and on macOS, where `--ignore-garbage`
  # does not exist and the redirect would leave an empty ~/.ssh/id_rsa behind.
  printf '%s' "${key_string}" | tr -d '\n\r' | base64 -d >~/.ssh/id_rsa
  chmod 600 ~/.ssh/id_rsa
}

function add_doc_server_ssh_keys() {
  local key_string="${1}"
  local server_url="${2}"
  local server_user="${3}"
  add_ssh_keys "${key_string}"
  printf 'Host %s\n\tStrictHostKeyChecking no\n\tUser %s\n' "${server_url}" "${server_user}" >>~/.ssh/config
}

# Deploy key for git@github.com. GH_PUSH_KEY is the base64 private key, same
# variable esp-at reads in add_github_ssh_keys.
function add_github_ssh_keys() {
  add_ssh_keys "${GH_PUSH_KEY}"
  printf 'Host github.com\n\tStrictHostKeyChecking no\n' >>~/.ssh/config
}

# Publish directory name for the docs of the current pipeline. This is the
# <version> segment of the deployed tree (<lang>/<version>/...), and it is also
# exported as MINIHUB_DOCS_VERSION when building, so the theme's version
# switcher (docs/_static/minihub_versions.js) pre-selects the published row.
#
#   master            -> latest
#   release/vX.Y.Z    -> vX.Y.Z
#   tag vX.Y.Z        -> vX.Y.Z
#   anything else     -> preview-<ref>   (branch or MR source branch)
#
# The tag case is a no-op today (workflow.rules does not create tag pipelines);
# it is here so enabling tags later does not need a second helper.
#
# Same idea as esp-docs sanitize_version(): everything outside
# [A-Za-z0-9._-] becomes '-', so the name is a legal URL path segment.
function minihub_docs_version() {
  local name
  if [ -n "${CI_COMMIT_TAG:-}" ]; then
    name="${CI_COMMIT_TAG}"
  elif [ "${CI_COMMIT_REF_NAME:-}" = "master" ]; then
    name="latest"
  elif [[ "${CI_COMMIT_BRANCH:-}" =~ ^release/v(.+)$ ]]; then
    name="v${BASH_REMATCH[1]}"
  else
    name="preview-${CI_COMMIT_REF_NAME:-unknown}"
  fi
  printf '%s' "${name}" | sed -E 's#[^A-Za-z0-9._-]#-#g'
}

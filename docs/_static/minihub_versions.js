/*
 * MiniHub docs version switcher data.
 *
 * Consumed by sphinx_idf_theme (the Espressif theme) through
 * DOCUMENTATION_OPTIONS.VERSIONS_URL, which conf_common.py sets from
 * `versions_url = "_static/minihub_versions.js"`. The theme fills the top-right
 * version <select> from DOCUMENTATION_VERSIONS.VERSIONS.
 *
 * MiniHub ships a single target (ESP32-S31), so the docs have NO <target>
 * segment in the URL: a version lives at <lang>/<version>/<page>.html. The
 * target switcher is therefore disabled (has_targets: false).
 *
 * To publish a new version:
 *   1. Build with MINIHUB_DOCS_VERSION=<ver> (e.g. v1.2.0) and publish it under
 *      <lang>/<ver>/ — see tools/publish_docs.sh.
 *   2. Add an entry to VERSIONS below. `name` MUST equal the MINIHUB_DOCS_VERSION
 *      used for that build, so the theme pre-selects the right row.
 *   3. Re-publish EVERY version, so each copy of this file lists all versions.
 */
var DOCUMENTATION_VERSIONS = {
  DEFAULTS: { has_targets: false },
  VERSIONS: [
    { name: "latest", has_targets: false }
    // { name: "v1.0.0", has_targets: false },
  ]
};

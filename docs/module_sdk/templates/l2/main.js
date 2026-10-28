"use strict";

/*
 * host is not a global: the runtime evaluates this file first, then calls the
 * global setup(host) below. Touching host at top level throws before setup runs,
 * and the module is skipped with a JS-exception log line.
 */
function setup(host) {
    host.log.info("PLACEHOLDER_ID: L2 plugin loaded");
}

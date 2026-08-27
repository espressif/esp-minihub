#!/usr/bin/env bash
# Thin idf.py wrapper: default flash baud 2Mbps (override with ESPBAUD or -b).
set -euo pipefail
export ESPBAUD="${ESPBAUD:-2000000}"
exec idf.py "$@"

#!/usr/bin/env python3
"""Host tests for the console's catalog-driven control logic.

The console is plain browser JS with no build step, so the tests run the real
web/js sources in Node's vm against a DOM stub. The catalog fixture is built
from the firmware generator's own runtime registry, so a type added to
model/devices.csv changes what these tests assert without editing them.

    python3 components/minihub_web/test/host/run_host_tests.py
"""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HOST = pathlib.Path(__file__).resolve().parent
ROOT = HOST.parents[3]
WEB_JS = ROOT / "components/minihub_web/web/js"
WIDGET_GEN = ROOT / "components/minihub_device_widget/tools/gen_device_widget.py"


def api_catalog(registry: list[dict], lang: str = "en") -> dict:
    """Reshape the runtime registry the way GET /api/device_widgets serves it."""
    devices: dict[str, dict] = {}
    parameters: dict[str, dict] = {}
    for row in registry:
        style = row["style"]
        param = {
            "parameter": row["parameter"],
            "label": (style.get("labels") or {}).get(lang) or row["parameter"],
            "widget": style.get("widget", ""),
            "image": "",
            "style": json.dumps(style, ensure_ascii=False),
        }
        dev = devices.setdefault(row["device"], {
            "device": row["device"],
            "image": row.get("device_image") or "",
            "params": [],
        })
        dev["params"].append(param)
        parameters.setdefault(row["parameter"], param)
    return {"devices": list(devices.values()), "parameters": parameters}


def main() -> int:
    node = shutil.which("node")
    if not node:
        print("error: node is required for the console host tests", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="web_host_test_") as tmp:
        out = pathlib.Path(tmp)
        gen = subprocess.run([sys.executable, str(WIDGET_GEN), "--out-dir", str(out)],
                             capture_output=True, text=True)
        if gen.returncode != 0:
            sys.stdout.write(gen.stdout + gen.stderr)
            return gen.returncode
        registry = json.loads((out / "ui_controls.json").read_text(encoding="utf-8"))["registry"]
        fixture = out / "catalog.json"
        fixture.write_text(json.dumps(api_catalog(registry), ensure_ascii=False), encoding="utf-8")
        print(f"catalog fixture: {len(registry)} runtime entries, "
              f"{len({r['device'] for r in registry})} types")
        env = dict(os.environ, MINIHUB_TEST_CATALOG=str(fixture), MINIHUB_WEB_JS=str(WEB_JS))
        return subprocess.run([node, "--test", str(HOST / "test_device_widgets.js")],
                              env=env).returncode


if __name__ == "__main__":
    raise SystemExit(main())

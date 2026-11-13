"""Drift gate for the BLE Mesh capability map.

The map is the single source of truth for model -> capability -> type -> parameter ->
command. These checks fail the build when it drifts from the widget catalog, from the
adapter that reads it, from the firmware's own catalog flattener, from the config the
repo publishes, or from the Bluetooth SIG characteristic sizes recorded in the plan.
"""

import csv
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAP = ROOT / "components/minihub_ble_mesh/config/ble_mesh_map.json"
MODEL = ROOT / "components/minihub_device_widget/model"
ADAPTER = ROOT / "components/minihub_protocol_adapters/adapter_ble_mesh/ble_mesh_adapter.c"
WIDGET_GEN = ROOT / "components/minihub_device_widget/tools/gen_device_widget.py"
WIDGET_FLATTENER = ROOT / "components/minihub_device_widget/src/minihub_device_widget_model.c"
ADAPTERS_JSON = ROOT / "configs/adapters.json"
DOC_EXAMPLES = [ROOT / "docs/en/plugins/develop_json.rst", ROOT / "docs/zh_CN/plugins/develop_json.rst"]

# Byte sizes per Bluetooth SIG GSS characteristic (local authority:
# bluetooth-SIG/gss/org.bluetooth.characteristic.*.yaml).
SIG_SIZE = {
    "0x004f": 1,  # Temperature 8
    "0x0075": 2,  # Temperature
    "0x0076": 2,  # Humidity
    "0x0055": 3,  # Illuminance
    "0x0042": 1,  # Percentage 8
    "0x004d": 1,  # Boolean
    "0x0077": 2,  # CO2 Concentration
    "0x00a9": 4,  # Pressure
}

# Mesh writers a control row may name (ble_mesh_map.h: minihub_bm_ctrl_kind_t).
CTRL_KINDS = {"onoff", "level", "lightness", "ctl", "hsl"}

# minihub_bm_ctrl_kind_t enumerator -> the `kind` string the map JSON uses.
CTRL_KIND_ENUM = {
    "ONOFF": "onoff",
    "LEVEL": "level",
    "LIGHTNESS": "lightness",
    "CTL": "ctl",
    "HSL": "hsl",
}


class TestBleMeshMap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = json.loads(MAP.read_text(encoding="utf-8"))
        with (MODEL / "devices.csv").open(newline="", encoding="utf-8") as fh:
            cls.device_rows = {r["device_id"]: r for r in csv.DictReader(fh)}
        cls.types = set(cls.device_rows)
        with (MODEL / "parameters.csv").open(newline="", encoding="utf-8") as fh:
            cls.param_rows = {r["parameter"]: r for r in csv.DictReader(fh)}
        cls.params = set(cls.param_rows)
        cls.adapter_src = ADAPTER.read_text(encoding="utf-8")

    def test_version(self):
        self.assertEqual(self.m["version"], 1)

    def test_classify_types_exist(self):
        for rule in self.m["classify"]:
            self.assertIn(rule["type"], self.types, rule)
            keys = [k for k in ("any_caps", "all_props", "any_props") if rule.get(k)]
            self.assertTrue(keys, rule)

    def test_classify_priority_unique(self):
        priorities = [r["priority"] for r in self.m["classify"]]
        self.assertEqual(len(priorities), len(set(priorities)), priorities)

    def test_param_names_exist(self):
        for key in ("params", "controls", "sensors"):
            for row in self.m[key]:
                self.assertIn(row["param"], self.params, (key, row))

    def test_sensor_sizes_match_sig(self):
        for row in self.m["sensors"]:
            prop = row["property"].lower()
            self.assertIn(prop, SIG_SIZE, row)
            self.assertEqual(row["size"], SIG_SIZE[prop], row)

    def test_sensor_bounds_match_catalog(self):
        # D2: the adapter clamps a decoded sensor value to the catalog range, so
        # the map must quote the same bounds the widget catalog does.
        for row in self.m["sensors"]:
            cat = self.param_rows[row["param"]]
            cat_min = float(cat["min"]) if cat["min"] else None
            cat_max = float(cat["max"]) if cat["max"] else None
            map_min = row.get("min")
            map_max = row.get("max")
            self.assertEqual(map_min, cat_min, row)
            self.assertEqual(map_max, cat_max, row)

    def test_sensor_properties_are_classified(self):
        classified = set()
        for rule in self.m["classify"]:
            classified.update(p.lower() for p in rule.get("all_props", []))
            classified.update(p.lower() for p in rule.get("any_props", []))
        for row in self.m["sensors"]:
            self.assertIn(row["property"].lower(), classified, row)

    def test_models_have_client(self):
        for row in self.m["models"]:
            self.assertRegex(row["id"], r"^0x[0-9a-f]{4}$")
            self.assertRegex(row["client"], r"^0x[0-9a-f]{4}$")
            self.assertTrue(row["cap"], row)

    def test_controls_reference_known_model(self):
        ids = {r["id"] for r in self.m["models"]}
        for row in self.m["controls"]:
            self.assertIn(row["model"], ids, row)

    def test_control_models_declare_a_get(self):
        # A model the Hub can write but never reads back leaves the console
        # showing the params[] init value as if the node had reported it.
        written = {r["model"] for r in self.m["controls"]}
        for row in self.m["models"]:
            if row["id"] in written:
                self.assertRegex(row.get("get", ""), r"^0x[0-9a-f]{4}$", row)

    def test_intensity_not_claimed(self):
        # D3: mesh has no intensity path; it must not be seeded or routed.
        for key in ("params", "controls"):
            self.assertNotIn("intensity", [r["param"] for r in self.m[key]], key)

    def test_control_kinds_known(self):
        for row in self.m["controls"]:
            self.assertIn(row["kind"], CTRL_KINDS, row)
            self.assertTrue(row.get("field"), row)
            if row.get("fallback"):
                self.assertIn(row["fallback"], CTRL_KINDS, row)

    def test_control_fields_unique_per_kind(self):
        # The adapter looks a param up by (kind, field); two rows sharing one
        # pair would make the lookup ambiguous.
        seen = set()
        for row in self.m["controls"]:
            key = (row["kind"], row["field"])
            self.assertNotIn(key, seen, row)
            seen.add(key)

    def test_cap_params_have_init(self):
        # A cap-keyed param is seeded into props.params, so it needs a default.
        for row in self.m["params"]:
            if row.get("cap"):
                self.assertIn("init", row, row)

    def test_writable_params_have_control(self):
        # Every RW param the adapter seeds must have a mesh writer behind it.
        controls = {r["param"] for r in self.m["controls"]}
        for row in self.m["params"]:
            if row.get("access") == "RW" and row.get("cap"):
                self.assertIn(row["param"], controls, row)

    def test_readable_params_have_a_read_path(self):
        # A readable param with no Get behind it keeps showing its init value
        # forever, which the console cannot tell from a value the node reported.
        gets = {r["cap"]: r.get("get", "") for r in self.m["models"]}
        props = {r["property"].lower() for r in self.m["sensors"]}
        for row in self.m["params"]:
            if "R" not in row.get("access", ""):
                continue
            if row.get("cap"):
                self.assertRegex(gets.get(row["cap"], ""), r"^0x[0-9a-f]{4}$", row)
            else:
                self.assertIn(row["prop"].lower(), props, row)

    def _model_by_config_name(self, name):
        """Resolve a config model name the way minihub_bm_model_id_by_name does."""
        for row in self.m["models"]:
            if name in (row["cap"], row.get("alias")):
                return row
        return None

    def _first_param_row(self, param):
        """minihub_bm_param_by_name returns the first matching row, not every row."""
        for row in self.m["params"]:
            if row["param"] == param:
                return row
        return None

    def _catalog_params(self, type_id):
        return [p.strip() for p in self.device_rows[type_id]["parameters"].split(",") if p.strip()]

    def test_model_config_names_unique(self):
        # One name must select one model: the lookup matches cap and alias alike,
        # so a collision between two rows makes a config declaration ambiguous.
        seen = {}
        for row in self.m["models"]:
            for name in (row["cap"], row.get("alias")):
                if not name:
                    continue
                self.assertNotIn(name, seen, (name, row, seen.get(name)))
                seen[name] = row["id"]

    def _check_bind_entry(self, entry, where):
        name = entry.get("name")
        if not name:
            return
        row = self._model_by_config_name(name)
        self.assertIsNotNone(row, f"{where}: model name '{name}' is not in models[]")
        self.assertEqual(entry["model_id"], row["id"], f"{where}: {entry}")

    def test_published_bind_models_resolve(self):
        cfg = json.loads(ADAPTERS_JSON.read_text(encoding="utf-8"))
        binds = cfg["adapters"]["ble_mesh"]["provision"]["bind_models"]
        self.assertTrue(binds)
        for entry in binds:
            self._check_bind_entry(entry, "configs/adapters.json")

    def test_doc_bind_model_examples_resolve(self):
        # The L1 pack example is copied verbatim by plugin authors; a name the
        # table does not know would fail at inject time, not at review time.
        pattern = re.compile(r'\{\s*"model_id":\s*"(0x[0-9a-fA-F]{4})"\s*,\s*"name":\s*"([^"]+)"\s*\}')
        found = 0
        for doc in DOC_EXAMPLES:
            for model_id, name in pattern.findall(doc.read_text(encoding="utf-8")):
                self._check_bind_entry({"model_id": model_id.lower(), "name": name}, doc.name)
                found += 1
        self.assertTrue(found, "no bind_models example found in the plugin docs")

    def test_control_fields_cover_the_adapter_lookups(self):
        # The adapter asks for a param by (kind, field) with a literal field name.
        # Renaming that field in the table silently disables the command, so the
        # requirement is read out of the adapter instead of restated here.
        lookups = re.findall(r'bm_control_param_for_field\(\s*MINIHUB_BM_CTRL_(\w+)\s*,\s*"([^"]+)"\s*\)',
                             self.adapter_src)
        self.assertTrue(lookups, "no bm_control_param_for_field() lookup found in the adapter")
        have = {(r["kind"], r["field"]) for r in self.m["controls"]}
        for enum, field in lookups:
            kind = CTRL_KIND_ENUM[enum]
            self.assertIn((kind, field), have, f"adapter looks up ({kind}, {field})")

    def test_hsl_fields_are_writable(self):
        # The HSL writer only fills the fields it compares against; any other
        # field on an hsl row is a param the Hub accepts but cannot send.
        fields = set(re.findall(r'strcmp\(ctrl->field,\s*"([^"]+)"\)', self.adapter_src))
        self.assertTrue(fields, "no ctrl->field comparison found in the adapter")
        for row in self.m["controls"]:
            if row["kind"] == "hsl":
                self.assertIn(row["field"], fields, row)

    def test_classify_types_are_registerable(self):
        # A type-only config declaration derives its caps from the catalog params
        # and its sensor properties from classify[]. A type with neither is
        # refused at registration, so it must not be advertised as classifiable.
        for rule in self.m["classify"]:
            type_id = rule["type"]
            caps = {self._first_param_row(p)["cap"]
                    for p in self._catalog_params(type_id)
                    if self._first_param_row(p) and self._first_param_row(p).get("cap")}
            props = rule.get("all_props", []) + rule.get("any_props", [])
            self.assertTrue(caps or props, f"{type_id} exposes no mapped parameter or property")

    def test_flat_catalog_primary_param_matches_model(self):
        # The adapter reads primary_param from the generated flat catalog, so the
        # column has to survive generation and still name a parameter of the type.
        with tempfile.TemporaryDirectory(prefix="dw_gate_") as tmp:
            proc = subprocess.run([sys.executable, str(WIDGET_GEN), "--out-dir", tmp],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            with (pathlib.Path(tmp) / "devices.csv").open(newline="", encoding="utf-8") as fh:
                flat = list(csv.DictReader(fh))
        self.assertTrue(flat)
        self.assertIn("primary_param", flat[0], "generated devices.csv lost the primary_param column")
        seen = set()
        for row in flat:
            dev = row["device"]
            seen.add(dev)
            self.assertIn(dev, self.device_rows, row)
            self.assertEqual(row["primary_param"], self.device_rows[dev]["primary_param"], row)
        self.assertEqual(seen, self.types)
        for dev in self.types:
            params = {r["parameter"] for r in flat if r["device"] == dev}
            primary = self.device_rows[dev]["primary_param"]
            if primary:
                self.assertIn(primary, params, dev)

    def test_both_flat_catalog_producers_write_the_same_columns(self):
        # The flat registry has two producers: the build-time generator and the
        # firmware flattener that runs on an uploaded model pair. A column only
        # one of them writes is a column the adapter loses after a remote update.
        with tempfile.TemporaryDirectory(prefix="dw_gate_") as tmp:
            proc = subprocess.run([sys.executable, str(WIDGET_GEN), "--out-dir", tmp],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            with (pathlib.Path(tmp) / "devices.csv").open(newline="", encoding="utf-8") as fh:
                generated = next(csv.reader(fh))
        src = WIDGET_FLATTENER.read_text(encoding="utf-8")
        written = re.findall(r'fputs\("([a-z_]+(?:,[a-z_]+)*)\\n", f\)', src)
        self.assertEqual(len(written), 1, "expected one flat-header fputs() in the flattener")
        self.assertEqual(written[0].split(","), generated)

    def test_adapter_has_no_param_hardcodes(self):
        # Tripwire for the defaults and the type gate the map now owns. The host
        # tests prove the behaviour; this catches a hardcode reintroduced on a
        # path they do not exercise.
        for literal in ("type_supports_onoff", '"22.0"', "int bright = 50", "int temp = 4000"):
            self.assertNotIn(literal, self.adapter_src, literal)


if __name__ == "__main__":
    unittest.main()

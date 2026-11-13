#!/usr/bin/env python3
"""Validate every `*_map.json` and enforce ONE shared param vocabulary.

The single source of truth is `components/minihub_device_widget/model/parameters.csv`
(see capability_map_schema.md). Every protocol map (ble_mesh, matter, zigbee)
emits only those normalized param names — that is what makes "add a device type
= edit JSON" work uniformly across protocols.

All three protocol maps now use the shared *shape* (`models`/`classify`/
`params`/`controls`/`sensors`). ble_mesh and matter additionally use the shared
*capability names* (`onoff`/`level`/`lightness`/`ctl`/`hsl`/`sensor`) and the
shared `params` section; zigbee keeps its fine-grained internal cap names
(`temp`/`humidity`/`ias_zone`/`electrical`/`energy`) because collapsing them to a
single `sensor` cap would break per-cluster param filtering without a loader
change. The *param* names surfaced to the widget MUST be the shared vocabulary
regardless. `test_all_maps_share_one_param_vocabulary` enforces that; the
shared-capability structural checks apply only to maps that also use the
`params` section.

Run:  python3 -m unittest tools.test_capability_map
"""

import csv
import glob
import json
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SHARED_CAPS = {"onoff", "level", "lightness", "ctl", "hsl", "sensor"}


def _load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _map_files():
    return sorted(
        p for p in glob.glob(os.path.join(ROOT, "components", "**", "*_map.json"),
                             recursive=True)
        if os.path.basename(p) != "build"
    )


def _catalog_params():
    """Normalized param names = the master vocabulary in parameters.csv.

    This is the single source of truth for widget-facing param names. Every
    protocol map MUST emit only these names.
    """
    csv_path = os.path.join(
        ROOT, "components", "minihub_device_widget", "model", "parameters.csv")
    params = set()
    if not os.path.isfile(csv_path):
        return params
    with open(csv_path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            name = (row.get("parameter") or "").strip()
            if name:
                params.add(name)
    return params


def _clean(d):
    return {k: v for k, v in d.items() if not k.startswith("_")}


def _is_shared_shape(m):
    """Uses the shared top-level shape (models/classify/params/controls/sensors)."""
    return "models" in m


def _uses_params_section(m):
    """Fully shared: shared shape AND the shared `params` capability section."""
    return "params" in m


# Capability names that are legitimately allowed in any map's `models`/`classify`.
KNOWN_CAPS = SHARED_CAPS | {
    "temp", "humidity", "ias_zone", "electrical", "energy",  # zigbee internal
}


class CapabilityMapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.maps = {os.path.relpath(p, ROOT): _load(p) for p in _map_files()}
        cls.catalog = _catalog_params()
        self = cls()
        self.assertGreaterEqual(len(cls.maps), 1, "no *_map.json found")

    # ---- shared-schema (ble_mesh, matter) validation ----------------------

    def test_shared_maps_have_version_and_models(self):
        for name, raw in self.maps.items():
            if not _is_shared_shape(raw):
                continue
            m = _clean(raw)
            self.assertIn("version", m, f"{name}: missing version")
            self.assertTrue(m.get("models"), f"{name}: missing models[]")

    def test_shared_models_use_known_caps_and_ids(self):
        for name, raw in self.maps.items():
            if not _is_shared_shape(raw):
                continue
            m = _clean(raw)
            for row in m.get("models", []):
                cap = row.get("cap")
                self.assertIn(cap, KNOWN_CAPS,
                              f"{name}: model cap {cap!r} unknown")
                if "also" in row:
                    self.assertIn(row["also"], SHARED_CAPS,
                                  f"{name}: 'also' {row['also']!r} unknown")
                mid = row.get("id")
                ok = (isinstance(mid, int)) or (
                    isinstance(mid, str) and mid[:2] in ("0x", "0X"))
                self.assertTrue(ok, f"{name}: bad model id {mid!r}")

    def test_shared_classify_types_in_catalog(self):
        for name, raw in self.maps.items():
            if not _uses_params_section(raw):
                continue
            m = _clean(raw)
            known = self._catalog_types()
            for row in m.get("classify", []):
                t = row.get("type")
                self.assertTrue(t, f"{name}: classify missing type")
                self.assertIn(t, known,
                              f"{name}: classify type {t!r} not in catalog")

    def test_shared_params_use_normalized_vocabulary(self):
        vocab = self.catalog
        for name, raw in self.maps.items():
            if not _uses_params_section(raw):
                continue
            m = _clean(raw)
            for row in m.get("params", []):
                p = row.get("param")
                self.assertTrue(p, f"{name}: param row missing 'param'")
                self.assertIn(p, vocab,
                              f"{name}: param {p!r} not in the shared "
                              f"device_widget vocabulary {sorted(vocab)}")
                self.assertTrue(
                    "cap" in row or "prop" in row,
                    f"{name}: param {p!r} needs 'cap' or 'prop'")

    def test_shared_controls_and_sensors_reference_params(self):
        for name, raw in self.maps.items():
            if not _uses_params_section(raw):
                continue
            m = _clean(raw)
            declared = {row.get("param") for row in m.get("params", [])}
            for row in m.get("controls", []):
                p = row.get("param")
                self.assertIn(p, declared,
                              f"{name}: control param {p!r} not in params[]")
                self.assertIn(row.get("kind"), SHARED_CAPS,
                              f"{name}: control kind {row.get('kind')!r} unknown")
            for row in m.get("sensors", []):
                p = row.get("param")
                self.assertIn(p, declared,
                              f"{name}: sensor param {p!r} not in params[]")

    def test_ble_mesh_and_matter_share_one_vocabulary(self):
        shared = {n: _clean(r) for n, r in self.maps.items() if _uses_params_section(r)}
        if len(shared) < 2:
            self.skipTest("need >=2 shared-schema maps")
        vocabs = {n: {row.get("param") for row in m.get("params", [])}
                  for n, m in shared.items()}
        base_name, base = next(iter(vocabs.items()))
        for n, v in vocabs.items():
            self.assertEqual(
                v, base,
                f"param vocabularies of {base_name} and {n} differ: "
                f"{sorted(v ^ base)}")

    # ---- zigbee divergence (known gap, recorded not silently passed) ------

    def test_all_maps_share_one_param_vocabulary(self):
        """Every map (ble_mesh, matter, zigbee) emits ONLY the shared vocabulary.

        The canonical vocabulary is parameters.csv. A protocol may use its own
        native id namespace (model/cluster/property) and its own cap names
        internally, but the param names it surfaces to the widget MUST be the
        same across all protocols. This is what makes "add a device type = edit
        JSON" work uniformly.
        """
        vocab = self.catalog
        self.assertTrue(vocab, "parameters.csv vocabulary is empty?")
        for name, raw in self.maps.items():
            m = _clean(raw)
            params = set()
            for row in m.get("params", []) + m.get("attrs", []) + m.get("sensors", []):
                p = row.get("param")
                if p:
                    params.add(p)
            for p in sorted(params):
                self.assertIn(
                    p, vocab,
                    f"{name}: param {p!r} is not in the shared vocabulary "
                    f"(parameters.csv) — add it there or rename")

    # ---- helpers ----------------------------------------------------------

    def _catalog_types(self):
        csv_path = os.path.join(
            ROOT, "components", "minihub_device_widget", "model", "devices.csv")
        types = set()
        if os.path.isfile(csv_path):
            with open(csv_path, "r", encoding="utf-8", newline="") as f:
                for row in csv.DictReader(f):
                    tid = (row.get("device_id") or "").replace('"', '').strip()
                    if tid:
                        types.add(tid)
        return types


if __name__ == "__main__":
    unittest.main()

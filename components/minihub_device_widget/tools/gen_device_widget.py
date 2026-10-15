#!/usr/bin/env python3
"""
minihub_device_widget data-model parser / generator.

The data model is FOUR plain CSV files under `model/` (source of truth,
git-diffable, meant to be extended by developers):

  model/devices.csv      device types            (device_id, parameters, ...)
  model/services.csv     RainMaker system services (service_id, rainmaker_type, ...)
  model/parameters.csv   parameter catalog       (parameter, label_zh, label_en,
                                  data_type, ui_element, ...)
  model/ui_elements.csv  closed UI enum          (id, label, description, binds)

They mirror the ESP RainMaker four layers Device -> Service -> Parameter ->
UI Element (see STANDARD_TYPES.md), exactly as upstream draws them:

  - Upstream lists a device type's parameters ON THE DEVICE
    (`esp.device.lightbulb` -> Name, Power*, Brightness, ...), so a device
    carries its own `parameters` list here too.
  - Upstream's `esp.service.*` table holds ONLY the SYSTEM services
    (ota / schedule / time / system); there is no `esp.service.lightbulb`.
    MiniHub is not in the RainMaker repo, so it re-declares those four here
    (with the upstream type kept in `rainmaker_type` for traceability). They
    are attached to a node/device independently of its type and their
    parameters are owned by the RainMaker core, so they are NOT part of the
    generated control registry - services.csv is reference data.

Design rule: **MiniHub does not know what a parameter means.** It only knows
the data model and the set of UI controls. A device is adapted by listing
parameter NAMES in `services.csv`; the UI control, bounds, label and CSS are
looked up BY NAME at generation time and bound again by name at runtime
(`minihub_device_widget_find(device, parameter)`). No code change is needed to
add a device.

Name-based binding in `parameters.csv`:
  - The catalog is keyed by parameter NAME only - there is no `device` column,
    so no per-device override table and nothing to inherit.
  - `visible`, `properties`, `min`, `max`, `step` and `unit` are all **defaults
    for the type**. A registered point (a plugin's `devices.json`) may override
    them per instance: `props.visible = false` hides a control, a plugin's
    `min`/`max` wins over the catalog's. The catalog only says what to do when
    the instance says nothing.
  - `image` is a repo-relative path (`./images/parameters/<name>.svg`), checked
    at generation time so a broken link fails the build, not the UI.

Generated output (never edit by hand; written under --out-dir, normally the
CMake binary dir — not committed under the component source tree):

  - ui_controls.json              structured output (devices/services/
                                  parameters/parameter_catalog/ui_elements/registry)
  - devices.csv                   FLAT runtime registry (device,parameter,
                                  style,image,device_image) -
                                  protocol-agnostic: device DISPLAY is
                                  protocol-independent, so the transport
                                  prefix lives in the device registration,
                                  not in this catalog. `device_image` is the
                                  device-class icon override repeated on
                                  every row of that device. Firmware seeds
                                  /data/config/devices.csv from the embedded
                                  C string at boot (not by flashing this file).
  - minihub_device_widget_gen.c   the same flat registry as an embedded
                                  fallback CSV for firmware

Usage:
    python3 tools/gen_device_widget.py --out-dir DIR   # model/ -> DIR
    python3 tools/gen_device_widget.py --check         # validate only

Stdlib only - no third-party dependency (no openpyxl).
"""
import argparse
import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
COMP = os.path.dirname(HERE)
MODEL_DIR = os.path.join(COMP, "model")

M_DEVICES = "devices"
M_SERVICES = "services"
M_PARAMS = "parameters"
M_UI = "ui_elements"

# Column order of each model CSV (row 1 header).
DEVICE_COLS = ["device_id", "parameters", "primary_param", "mandatory_params", "image"]
# RainMaker system services, re-declared for MiniHub (see the module docstring):
# reference data, NOT part of the generated control registry.
SERVICE_COLS = ["service_id", "parameters"]
# The catalog is keyed by parameter NAME only: it says how a parameter is
# rendered, not whether a given device shows it. `visible` is an INSTANCE
# property (a registered point's props.visible), so it is deliberately not a
# column here - and there is no `device` column either, so the catalog stays a
# single global lookup instead of a per-device override table.
PARAM_COLS = ["parameter", "label_zh", "label_en", "data_type", "properties", "min",
              "max", "step", "unit", "ui_element", "options", "description",
              "visible", "css", "image", "requires"]
UI_COLS = ["id", "label", "description", "binds"]

# CSV label column -> language code used in style_json.labels and in the
# ?lang= query of /api/device_widgets. This is the list for the Python side
# (check_catalog_i18n.py imports it), so adding a language means one entry here
# plus one column in parameters.csv. The C flattener keeps its own copy of the
# codes (minihub_device_widget_model.c, dw_build_style) because it cannot import
# this file -- change both when adding a language.
LABEL_COLS = {"label_zh": "zh", "label_en": "en"}

MODEL_FILES = {
    M_DEVICES: (os.path.join(MODEL_DIR, "devices.csv"), DEVICE_COLS),
    M_SERVICES: (os.path.join(MODEL_DIR, "services.csv"), SERVICE_COLS),
    M_PARAMS: (os.path.join(MODEL_DIR, "parameters.csv"), PARAM_COLS),
    M_UI: (os.path.join(MODEL_DIR, "ui_elements.csv"), UI_COLS),
}

# Flat columns of the emitted runtime registry (MUST match the C loader, which
# resolves columns by header name and has NO `visible` column: visibility is an
# INSTANCE property of the registered point (props.visible), never of the type).
# No `protocol` either: the catalog describes how a device TYPE is rendered,
# which is protocol-independent. The transport prefix (thread/zigbee/...) is a
# property of the device REGISTRATION (see minihub_point_resolve_by_type_param).
# `device_image` is the device-class icon override; it is repeated on every row
# of a device because the registry is flat.
CSV_COLS = ["device", "parameter", "style", "image", "device_image"]

# Closed web-widget set. Spectrum look (hue rainbow, sat, cct) is CSS tone from
# the parameter name — not extra ui.* types.
CLOSED_UI = {
    "ui.toggle", "ui.slider", "ui.dropdown", "ui.text", "ui.hidden", "ui.button",
}

# ui.* -> web widget id (upstream `esp.` prefix already stripped, like
# `esp.param.power` -> `power`). Deprecated hue-* aliases map to slider;
# the frontend picks the rainbow tone from parameter name `hue`.
UI_WIDGET = {
    "ui.toggle": "toggle",
    "ui.slider": "slider",
    "ui.dropdown": "dropdown",
    "ui.text": "text",
    "ui.hidden": "hidden",
    "ui.button": "button",
    # Removed from ui_elements.csv; kept so old catalogs still compile.
    "ui.hue-slider": "slider",
    "ui.hue-circle": "slider",
}


def ui_to_widget(ui):
    if ui in UI_WIDGET:
        return UI_WIDGET[ui]
    if ui and ui.startswith("ui."):
        return ui.split(".", 1)[-1].replace("-", "_")
    return "text"


def _uv(x):
    return str(x).strip().lower() in ("", "true", "1", "yes", "y", "是")


def _s(v):
    """Cell value -> stripped string. Empty cells (None) become "" and NOT the
    literal "None" (a plain str(None) would silently corrupt keys)."""
    if v is None:
        return ""
    return str(v).strip()


# --------------------------------------------------------------------------
# model reader
#   dev_rows  : list[dict]                    raw devices.csv rows
#   services  : list[{service_id, rainmaker_type, parameters:[...]}]
#   param_defs: dict[parameter] -> row dict
#   ui_rows   : list[dict]                    raw ui_elements.csv rows
# --------------------------------------------------------------------------

def _read_model_csv(path, cols):
    if not os.path.exists(path):
        raise SystemExit(f"model file not found: {path}")
    with open(path, newline="", encoding="utf-8") as f:
        rdr = csv.DictReader(f)
        missing = [c for c in cols if c not in (rdr.fieldnames or [])]
        if missing:
            raise SystemExit(f"{path}: missing column(s): {', '.join(missing)}")
        return [{c: ("" if r.get(c) is None else r.get(c)) for c in cols}
                for r in rdr]


def read_model():
    dev_rows = _read_model_csv(*MODEL_FILES[M_DEVICES])
    svc_rows = _read_model_csv(*MODEL_FILES[M_SERVICES])
    par_rows = _read_model_csv(*MODEL_FILES[M_PARAMS])
    ui_rows = _read_model_csv(*MODEL_FILES[M_UI])
    _check_image_links(dev_rows, "devices.csv")
    _check_image_links(par_rows, "parameters.csv")
    return _finish(dev_rows, svc_rows, par_rows, ui_rows)


def _finish(dev_rows, svc_rows, par_rows, ui_rows):
    # Closed ui_elements.csv must list exactly CLOSED_UI (no hue-* types).
    seen_ui = set()
    for r in ui_rows:
        uid = _s(r.get("id"))
        if not uid:
            raise SystemExit("ui_elements.csv: row with empty id")
        if uid not in CLOSED_UI:
            raise SystemExit(
                f"ui_elements.csv: '{uid}' is not in the closed set "
                f"({', '.join(sorted(CLOSED_UI))})")
        if uid in seen_ui:
            raise SystemExit(f"ui_elements.csv: duplicate id '{uid}'")
        seen_ui.add(uid)
    missing_ui = CLOSED_UI - seen_ui
    if missing_ui:
        raise SystemExit(
            f"ui_elements.csv: missing closed id(s): {', '.join(sorted(missing_ui))}")

    # global catalog, keyed by parameter name
    param_defs = {}
    for r in par_rows:
        key = _s(r.get("parameter"))
        if not key:
            raise SystemExit("Parameters: row with empty parameter")
        if key in param_defs:
            raise SystemExit(f"Parameters: duplicate entry '{key}'")
        ui_el = _s(r.get("ui_element"))
        if ui_el and ui_el not in CLOSED_UI and ui_el not in UI_WIDGET:
            raise SystemExit(
                f"parameters.csv: '{key}' ui_element '{ui_el}' is unknown "
                f"(closed set or deprecated hue-* alias)")
        param_defs[key] = r

    for key, r in param_defs.items():
        req = parse_requires_value(r.get("requires"))
        if not req:
            continue
        check_requires_names(key, req, param_defs)

    services = []
    for r in svc_rows:
        sid = _s(r.get("service_id"))
        if not sid:
            raise SystemExit("Services: row needs service_id")
        params = [p.strip() for p in _s(r.get("parameters")).split(",") if p.strip()]
        services.append({"service_id": sid, "parameters": params})
    return dev_rows, services, param_defs, ui_rows


def _check_image_links(rows, where):
    """`image` holds a repo-relative path (./images/...). Fail the build on a
    broken link instead of shipping a registry that points at nothing."""
    for r in rows:
        img = _s(r.get("image"))
        if not img:
            continue
        if not img.startswith("./images/"):
            raise SystemExit(f"{where}: image '{img}' must start with './images/'")
        if not os.path.isfile(os.path.join(COMP, img[2:])):
            raise SystemExit(f"{where}: image '{img}' does not exist "
                             f"(looked at {os.path.join(COMP, img[2:])})")


# --------------------------------------------------------------------------
# name-based binding
# --------------------------------------------------------------------------

def resolve(param_defs, dev, param):
    """Global catalog entry for `param`.

    This is the whole point of the data model: a device only lists parameter
    NAMES, and the UI control / label / bounds are attached here BY NAME.
    Interactive look lives in shared CSS widgets (device_widgets.css), not
    per-parameter SVG mockups.
    """
    row = param_defs.get(param)
    if row is None:
        raise SystemExit(
            f"parameters.csv: no catalog entry for parameter '{param}' "
            f"(used by device '{dev}'). Add a row for it.")
    merged = {k: ("" if v is None else v) for k, v in row.items()}
    merged["device"] = dev
    merged["parameter"] = param
    return merged


def _csv_num(v):
    """Preserve fractional catalog bounds (e.g. current_a step 0.01)."""
    f = float(v)
    i = int(f)
    return i if f == i else f


def _access_from_properties(props):
    """Map parameters.csv `properties` to instance-style access (R / RW)."""
    p = _s(props).lower().replace(" ", "")
    if not p:
        return None
    if "write" in p:
        return "RW"
    if "read" in p:
        return "R"
    return None


def _requires_name_list(op, names, seen):
    if not isinstance(names, list) or not names:
        raise SystemExit(f"parameters.csv: requires {op} must be a non-empty name list")
    clean = []
    for n in names:
        if not isinstance(n, str) or not n.strip():
            raise SystemExit(f"parameters.csv: requires {op} entries must be parameter names")
        n = n.strip()
        if n in seen or n in clean:
            raise SystemExit(f"parameters.csv: requires lists '{n}' more than once")
        clean.append(n)
    seen.extend(clean)
    return clean


def parse_requires_value(raw):
    """Catalog/instance `requires`.

    A parameter name (the default) is the same as ``{"all": [name]}``.
    ``{"all": [...]}`` is AND, ``{"any": [...]}`` is OR. ``{"not": [...]}``
    disables the control while any listed parameter is explicitly on.
    ``not`` may stand alone or sit beside ``all`` or ``any`` (not both).
    Empty is no precondition.
    """
    text = _s(raw)
    if not text:
        return None
    if not text.startswith("{"):
        return text
    try:
        obj = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"parameters.csv: requires is not valid JSON: {exc}")
    if not isinstance(obj, dict):
        raise SystemExit("parameters.csv: requires object must use all, any, and/or not")
    extra = set(obj) - {"all", "any", "not"}
    if extra or ("all" in obj and "any" in obj) or not obj:
        raise SystemExit(
            "parameters.csv: requires must be a parameter name, "
            '{"all":[...]}, {"any":[...]}, {"not":[...]}, '
            "or all/any together with not"
        )
    seen = []
    out = {}
    for op in ("all", "any", "not"):
        if op not in obj:
            continue
        out[op] = _requires_name_list(op, obj[op], seen)
    return out


def check_requires_names(key, req, known):
    groups = [[req]] if isinstance(req, str) else list(req.values())
    for names in groups:
        for n in names:
            if n == key or n not in known:
                raise SystemExit(
                    f"parameters.csv: '{key}' requires unknown parameter '{n}'")


def build_labels(pd):
    """style_json.labels: {lang: text}, empty cells omitted.

    There is deliberately no flat `label` key. The console reads the top-level
    `label` that /api/device_widgets picks per request, and a flat Chinese
    `label` here would win over it in the card renderer."""
    labels = {}
    for col, code in LABEL_COLS.items():
        text = _s(pd.get(col))
        if text:
            labels[code] = text
    return labels


def build_style(pd):
    widget = ui_to_widget(_s(pd.get("ui_element")))
    style = {"widget": widget, "labels": build_labels(pd)}
    for k in ("min", "max", "step"):
        v = pd.get(k)
        if v not in (None, ""):
            try:
                style[k] = _csv_num(v)
            except ValueError:
                pass
    unit = _s(pd.get("unit"))
    if unit:
        style["unit"] = unit
    access = _access_from_properties(pd.get("properties"))
    if access:
        style["access"] = access
    if _s(pd.get("options")):
        try:
            style["options"] = json.loads(pd["options"])
        except json.JSONDecodeError:
            pass
    css = _s(pd.get("css"))
    if css:
        style["css"] = css
    ui_el = _s(pd.get("ui_element"))
    if ui_el:
        style["ui_element"] = ui_el
    # Empty: no precondition. A name is {"all":[name]}. {"all":[...]} is AND,
    # {"any":[...]} is OR, {"not":[...]} disables while any listed name is on.
    # `not` may accompany all or any. UI disables a writable control when that
    # condition fails.
    req = parse_requires_value(pd.get("requires"))
    if req:
        style["requires"] = req
    return style


def device_parameters(dev_row):
    """Parameter NAMES a device type exposes, in declaration order."""
    return [p.strip() for p in _s(dev_row.get("parameters")).split(",") if p.strip()]


def build_runtime_entries(dev_rows, param_defs):
    """Flatten Device->Parameter into the flat runtime registry
    (device, parameter, style, image, device_image).

    Upstream lists a device type's parameters ON THE DEVICE, so `devices.csv`
    carries them and services are the orthogonal SYSTEM services (ota /
    schedule / time / system), which never contribute controls.
    Deliberately no `protocol`: the catalog is a TYPE descriptor for rendering,
    which is protocol-independent; the transport comes from the registration."""
    entries = []
    for d in dev_rows:
        dev = _s(d.get("device_id"))
        if not dev:
            raise SystemExit("devices.csv: row with empty device_id")
        dev_img = _s(d.get("image"))
        params = device_parameters(d)
        if not params:
            raise SystemExit(f"devices.csv: device '{dev}' lists no parameters")
        for p in params:
            pd = resolve(param_defs, dev, p)
            entries.append({
                "device": dev,
                "parameter": p,
                "visible": _uv(pd.get("visible")),
                "style": build_style(pd),
                "image": _s(pd.get("image")),
                "device_image": dev_img,
            })
    return entries


# --------------------------------------------------------------------------
# emitters
# --------------------------------------------------------------------------

def _spec(pd):
    return {
        "labels": build_labels(pd),
        "data_type": _s(pd.get("data_type")),
        "properties": _s(pd.get("properties")),
        "min": (_csv_num(pd["min"]) if pd.get("min") not in (None, "") else None),
        "max": (_csv_num(pd["max"]) if pd.get("max") not in (None, "") else None),
        "step": (_csv_num(pd["step"]) if pd.get("step") not in (None, "") else None),
        "unit": _s(pd.get("unit")) or None,
        "ui_element": _s(pd.get("ui_element")) or None,
        "widget": ui_to_widget(_s(pd.get("ui_element"))),
        "options": (json.loads(pd["options"]) if _s(pd.get("options")) else None),
        "description": _s(pd.get("description")) or None,
        "css": _s(pd.get("css")) or None,
        "image": _s(pd.get("image")) or None,
        "requires": parse_requires_value(pd.get("requires")),
    }


def emit_json(dev_rows, services, param_defs, ui_rows, entries, path):
    devices = []
    for d in dev_rows:
        dd = {k: ("" if v is None else v) for k, v in d.items()}
        dd["parameters"] = device_parameters(d)
        devices.append(dd)

    # global catalog: how a bare parameter NAME is rendered (auto-binding)
    catalog = {p: _spec(pd) for p, pd in sorted(param_defs.items())}

    # resolved per (device, parameter) - what the runtime actually renders
    parameters = {}
    for e in entries:
        pd = resolve(param_defs, e["device"], e["parameter"])
        parameters.setdefault(e["device"], {})[e["parameter"]] = _spec(pd)
    if catalog:
        parameters.setdefault("", {}).update(catalog)

    ui_elements = [{"id": _s(r.get("id")),
                    "label": _s(r.get("label")),
                    "description": _s(r.get("description")),
                    "binds": _s(r.get("binds"))} for r in ui_rows]

    payload = {
        "devices": devices,
        "services": services,
        "parameters": parameters,
        "parameter_catalog": catalog,   # name -> default UI binding
        "ui_elements": ui_elements,
        "registry": entries,            # flat runtime rows
    }
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    print(f"  wrote {path}")


def _flat_rows(entries):
    for e in entries:
        yield [e["device"], e["parameter"],
               json.dumps(e["style"], ensure_ascii=False),
               e["image"], e["device_image"]]


def emit_csv(entries, path):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    # LF, not the csv module's default CRLF: review / cross-platform diffs stay clean.
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(CSV_COLS)
        w.writerows(_flat_rows(entries))
    print(f"  wrote {path}  (flat runtime registry)")


def emit_c(entries, path):
    import io
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(CSV_COLS)
    w.writerows(_flat_rows(entries))
    lines = [
        "/* AUTO-GENERATED by tools/gen_device_widget.py from the model CSVs",
        " * (model/devices.csv, services.csv, parameters.csv, ui_elements.csv).",
        " * Do not edit by hand - edit the model CSVs and rebuild (CMake runs",
        " * this generator into the component binary dir).",
        " * This is the compiled-in FALLBACK registry (embedded CSV). The runtime",
        " * loader prefers /data/config/devices.csv when present, so new device",
        " * types can be added without recompiling. Same column order as CSV_COLS. */",
        "#include \"minihub_device_widget.h\"",
        "",
        "/* Embedded fallback CSV (RFC4180). Parsed at runtime by the loader. */",
        "const char MINIHUB_DW_EMBEDDED_CSV[] =",
        json.dumps(buf.getvalue()) + ";",
        "",
    ]
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"  wrote {path}  (embedded fallback CSV)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="validate only")
    ap.add_argument("--out-dir", metavar="DIR",
                    help="write devices.csv, ui_controls.json, and "
                         "minihub_device_widget_gen.c here (CMake binary dir)")
    args = ap.parse_args()

    if not args.check and not args.out_dir:
        raise SystemExit(
            "error: --out-dir DIR is required (or pass --check). "
            "Outputs go to the build tree, not components/.../generated/.")

    out_dir = os.path.abspath(args.out_dir) if args.out_dir else None

    dev_rows, services, param_defs, ui_rows = read_model()
    # Fail the build on catalog / localisation drift instead of shipping a console
    # that silently renders one language. Runs for --check too: that is the lint pass.
    import check_catalog_i18n
    i18n_errors, i18n_warnings = check_catalog_i18n.run()
    for w in i18n_warnings:
        print(f"  warn  {w}")
    if i18n_errors:
        for e in i18n_errors:
            print(f"  ERROR {e}")
        raise SystemExit(f"catalog i18n check failed: {len(i18n_errors)} error(s)")
    entries = build_runtime_entries(dev_rows, param_defs)
    print(f"parsed {len(entries)} runtime entries from model/ "
          f"({len(param_defs)} catalog parameters, "
          f"{len(services)} services)")
    if args.check:
        print("OK")
        return
    emit_json(dev_rows, services, param_defs, ui_rows, entries,
              os.path.join(out_dir, "ui_controls.json"))
    emit_csv(entries, os.path.join(out_dir, "devices.csv"))
    emit_c(entries, os.path.join(out_dir, "minihub_device_widget_gen.c"))
    print("done.")


if __name__ == "__main__":
    main()

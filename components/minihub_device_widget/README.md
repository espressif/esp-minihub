# minihub_device_widget

[English](README.md) | [中文](README_CN.md) · [Standard types](STANDARD_TYPES.md)

Type-catalog for device UI: which parameters a type exposes, and how each
parameter renders (widget, label, bounds, CSS). Source of truth:
`model/*.csv`. Build embeds a flat registry; icons stage to littlefs
`/data/images`.

**Rules**

- Catalog is keyed by **parameter name** (no per-device override column, no
  `visible` column).
- **One device = one point** (`<protocol>.<device>`); live values / access /
  visibility / instance bounds live in `props.params.<name>`.
- Catalog style = defaults; instance `devices.json` keys override when present.
- `ui_element` is a **closed set**: `ui.toggle` / `ui.slider` / `ui.dropdown` /
  `ui.text` / `ui.hidden`.
- Transport prefix comes from registration, not this catalog (protocol-agnostic).

## `model/*.csv`

### `devices.csv`

| Column | Content |
|--------|---------|
| `device_id` | Type id, e.g. `light`, `switch` |
| `parameters` | Comma-separated parameter names |
| `primary_param` | e.g. `power` |
| `mandatory_params` | Comma-separated mandatory names |
| `image` | Optional device-icon file name under `images/devices/` |

### `parameters.csv`

| Column | Content |
|--------|---------|
| `parameter` | Key without `esp.param.` prefix |
| `label_zh` | Chinese label — the default language and the fallback |
| `label_en` | English label — empty falls back to `label_zh` |
| `data_type` | `Bool` / `Int` / `String` / … |
| `properties` | e.g. `Read/Write` |
| `min` / `max` / `step` / `unit` | Slider defaults |
| `ui_element` | Closed set above |
| `options` | JSON array for `ui.dropdown`; each item carries `labels: {"zh":…, "en":…}` instead of a flat `label` |
| `description` | Free text |
| `css` | Renderer CSS |
| `image` | Optional control-icon file name under `images/parameters/` |
| `requires` | Optional precondition. A parameter name (default, e.g. `power`, same as `{"all":["power"]}`), or JSON: `{"all":[...]}` (AND), `{"any":[...]}` (OR), `{"not":[...]}` (disable while any listed name is on). `not` may accompany `all` or `any`, not both. Empty = no precondition. Quote the object in the CSV cell. |

Duplicate parameter names, or a device listing an unknown name → generator error.

### `ui_elements.csv`

Documents the closed `ui.*` ids (mapped to web `toggle` / `slider` / `dropdown` /
`text` / `hidden`). `hue` / `saturation` / `cct` are `ui.slider`; rainbow / tone
comes from parameter name + CSS, not extra widget types. Metering params
(`voltage_v` / `current_a` / `power_w` / `energy_wh`) are `ui.text` with
catalog `properties: Read` → style `access: R` (read-only value + unit).

| | `ui.hidden` | Instance `visible:false` |
|--|-------------|--------------------------|
| Where | `parameters.csv` `ui_element` | Plugin `devices.json` |
| Meaning | Type never draws a control | This device hides that param (still in `props.params`) |

Read-only sensors: instance `access:"R"` (still shown, no drag control).

### `services.csv`

Reference only (RainMaker system services). Not part of the widget registry.

### Seeded types

- **`light`** — `power`, `brightness`, `cct`, `hue` (label 颜色), `saturation`,
  `intensity`
- **`switch`** — `power`, `voltage_v`, `current_a`, `power_w`, `energy_wh`
- **`speaker`** — `power`, `volume` (`volume` requires `power`)
- **`name`** — in catalog; no device lists it → never a control

Full tables: [`STANDARD_TYPES.md`](STANDARD_TYPES.md).

<a id="icons"></a>
## Icons

Not compiled into firmware. Build stages `images/` → `/data/images`.

| Dir | Role | Convention |
|-----|------|------------|
| `images/devices/` | Device avatar | `<device>.svg` |
| `images/parameters/` | Optional control icon | `<device>_<parameter>.svg` or `<parameter>.svg` |

Interactive controls are CSS widgets in `minihub_web` (`device_widgets.js` /
`.css`). Resolver: `minihub_device_widget_image_name(device, parameter, …)` →
path relative to `/data/images`. Served as `GET /img/<subdir>/<name>`; URLs also
appear in `/api/device_widgets`.

## Generator

```bash
python3 tools/gen_device_widget.py --check
python3 tools/gen_device_widget.py --out-dir /tmp/dw_out
python3 tools/gen_svgs.py          # optional: regenerate device SVGs; --png needs cairosvg
```

CMake writes build-dir outputs only (do not commit): flat `devices.csv`,
`ui_controls.json`, `minihub_device_widget_gen.c`. Stdlib-only scripts.

<a id="remote-catalog"></a>
## Remote catalog (no reflash)

| Mode | How |
|------|-----|
| Firmware embed | Edit `model/*.csv` → rebuild → flash |
| Remote update | `PUT` model `devices.csv` + `parameters.csv` (`CONFIG_MINIHUB_WEB_UI`, same auth as REST) |

| File | Device path |
|------|-------------|
| Uploaded model CSVs | `/data/config/model/devices.csv`, `parameters.csv` |
| Flat runtime registry | `/data/config/devices.csv` |

When both model files are present, firmware flattens (same rules as
`gen_device_widget.py`), writes the runtime CSV, and
`minihub_device_widget_reload()` — no reboot. Invalid uploads are rejected.
Catalog updates types only; devices still need a registered point.

```bash
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  --data-binary @components/minihub_device_widget/model/devices.csv \
  http://$HUB/api/dev/catalog/devices.csv
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  --data-binary @components/minihub_device_widget/model/parameters.csv \
  http://$HUB/api/dev/catalog/parameters.csv
```

Local panel (no curl):

```bash
python3 tools/dev_sync.py --open
```

See [`tools/README.md`](../../tools/README.md).

## C API

```c
#include "minihub_device_widget.h"

const minihub_device_widget_device_t *d = minihub_device_widget_get("light");
const minihub_device_widget_entry_t *e =
    minihub_device_widget_find("light", "brightness");

char icon[MINIHUB_DW_IMAGE_PATH_MAX];
minihub_device_widget_image_name("light", "brightness", icon, sizeof(icon));
minihub_device_widget_image_name("light", NULL, icon, sizeof(icon));  // device avatar

char path[MINIHUB_POINT_ID_MAX];
minihub_point_resolve_by_type_param("light", "power", path, sizeof(path));
// -> device point id (e.g. "sim.js_kitchen"); param is in props.params
```

## Edit / add type

1. Edit `model/devices.csv` (parameter list) and `parameters.csv` (new names only).
2. Optional: `images/devices/<device>.svg`.
3. `python3 tools/gen_device_widget.py --check` then rebuild Host.
4. Commit `model/*` and `images/*` only.

## Add UI control

Closed set is small. If you must add one:

1. `model/ui_elements.csv` + generator `CLOSED_UI` / `UI_WIDGET`
2. Render in [`../minihub_web/web/js/device_widgets.js`](../minihub_web/web/js/device_widgets.js)

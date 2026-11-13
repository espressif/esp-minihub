# Capability map schema (shared across protocol adapters)

Every device protocol adapter (ble_mesh, zigbee, matter, ...) turns
protocol-native identifiers (SIG model IDs, ZCL cluster/attr IDs, Matter
cluster IDs) into the **same normalized parameter space** that
`minihub_device_widget` owns. The translation is data, not code: a
`*_map.json` file per protocol. Adding a device type should be a JSON edit,
not a C change.

This file is the contract. `tools/test_capability_map.py` checks every
`*_map.json` against it.

## Single source of truth

- The **normalized parameter names** live ONLY in
  `components/minihub_device_widget/model/parameters.csv` (the `parameter`
  column). That file is the canonical vocabulary. A map may emit ONLY those
  names — do NOT let a protocol invent its own (e.g. `temp` vs `temperature`,
  `battery_low` should be the shared `battery_low` row, etc.). `zone_status` and
  `battery_low` are already rows there so Zigbee's params are fully normalized.
- The **protocol-native IDs** (model / cluster / property) live in each map.
- **Capability names** (`onoff` / `level` / `lightness` / `ctl` / `hsl` /
  `sensor`) are an internal classification hint. Each protocol may use
  protocol-flavored cap names internally — Zigbee keeps `temp`/`humidity`/
  `ias_zone`/`electrical`/`energy` for **fine-grained per-cluster param
  filtering** (a temp sensor must not be offered energy params), which a single
  coarse `sensor` cap could not express without switching the loader to
  cluster-based filtering. They map onto the shared concepts above. The
  LOAD-BEARING unification is the param vocabulary, which IS shared across all
  maps.
- `Thread` has NO map: `thread_adapter.c` is RF/BR only; Matter-over-Thread
  devices are classified by the Matter map.

## Top-level shape

```jsonc
{
  "version": 1,                 // integer
  "models":    [ ... ],         // protocol-native id -> capability
  "classify":  [ ... ],         // capability/property set -> device type
  "params":    [ ... ],         // capability|property -> normalized param
  "controls":  [ ... ],         // param -> which id/field to write
  "sensors":   [ ... ]          // property -> normalized param decode
}
```

Keys beginning with `_` (e.g. `_comment`) are ignored.

## Sections

### `models` — protocol-native id -> capability
| field   | meaning |
|---------|---------|
| `id`    | native id, hex string (`"0x1000"`) or int, 0..0xFFFF |
| `cap`   | capability name (one of the set below); sets the primary cap bit |
| `also`  | optional second capability OR-ed onto the same id |
| `client`| the matching client-side id (same namespace as `id`) |
| `alias` | human label |

Known capabilities: `onoff`, `level`, `lightness`, `ctl`, `hsl`, `sensor`.
(ble_mesh already splits `ctl`/`hsl` out of ColorControl; Matter keeps them
on one cluster via `also`.)

### `classify` — pick a device type
Highest `priority` wins. `any_caps` is an array of capability names (match if
the device has ANY); `all_props` / `any_props` are arrays of native ids
(match if ALL / ANY present).
| field        | meaning |
|--------------|---------|
| `type`       | device type string (must exist in the type catalog) |
| `priority`   | int; larger wins |
| `any_caps`   | array of capability names |
| `all_props`  | array of native ids (all required) |
| `any_props`  | array of native ids (any required) |

### `params` — capability | property -> normalized param
Exactly one of `cap` (capability name) or `prop` (native id) must be present.
| field    | meaning |
|----------|---------|
| `cap`    | capability name |
| `prop`   | native id (sensor property) |
| `param`  | **normalized** param name (must be in the shared vocabulary) |
| `access` | `"RW"` or `"R"` |
| `init`   | initial value (string or number) for RW params |

### `controls` — param -> what to write
| field     | meaning |
|-----------|---------|
| `param`   | normalized param (must be declared in `params`) |
| `kind`    | capability name (drives the write path) |
| `model`   | native id to write to |
| `fallback`| optional capability used when `kind` is unavailable |
| `field`   | the field/attribute name within the target |

### `sensors` — property -> decode
| field             | meaning |
|-------------------|---------|
| `property`        | native id |
| `param`           | **normalized** param name |
| `size`            | byte length of the raw value |
| `signed`          | bool |
| `scale`           | multiply raw value by this |
| `unknown`         | optional sentinel meaning "no reading" |
| `bool_threshold`  | optional: treat raw >= this as true |
| `min` / `max`     | optional clamp bounds |

## Cross-protocol rules enforced by the test

1. All maps use the **same** normalized param names (the union must be a
   subset of the type-catalog vocabulary).
2. Every `controls[].param` and `sensors[].param` is declared in that map's
   `params`.
3. `models[].cap`/`also` and `controls[].kind` are known capability names.
4. `classify[].type` exists in the type catalog; native ids are well-formed.

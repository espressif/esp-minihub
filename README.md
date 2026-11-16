# MiniHub

[English](README.md) | [中文](README_CN.md)

ESP32 Host firmware for multi-protocol IoT hubs: Web UI, REST, WebSocket events,
Store modules (`.mhub`), and Host adapters (Zigbee / BLE Mesh / Matter / MQTT /
Modbus / Thread Border Router).

Third-party ecosystems ship as **Store plugins** (L1 / L2 / L3), not Host Core
dependencies. Public Host `store_preinstall` stays empty.

**License:** [Apache 2.0](LICENSE)

## Who this repo is for

| Goal | Start here | You should not need to… |
|------|------------|-------------------------|
| Understand architecture | [Introduction](https://docs.espressif.com/projects/esp-minihub/en/latest/introduction.html) · [Architecture](https://docs.espressif.com/projects/esp-minihub/en/latest/architecture.html) | Read Core `.c` |
| Build and flash the reference Host | [Getting started](https://docs.espressif.com/projects/esp-minihub/en/latest/getting_started.html) | Fork `components/minihub_*` |
| Ship an OEM Host product | [Host customization](https://docs.espressif.com/projects/esp-minihub/en/latest/host_customization.html) → [Production](https://docs.espressif.com/projects/esp-minihub/en/latest/production.html) | Copy Core into `application/` |
| Develop a Store plugin (L1/L2/L3) | [Plugin overview](https://docs.espressif.com/projects/esp-minihub/en/latest/plugins/overview.html) · [examples/](examples/README.md) | Change Host bring-up |
| REST / MCP / rules | [Data model](https://docs.espressif.com/projects/esp-minihub/en/latest/data_model.html) · [Rules](https://docs.espressif.com/projects/esp-minihub/en/latest/rules.html) · [API](https://docs.espressif.com/projects/esp-minihub/en/latest/api/index.html) | Implement adapters in C |
| Radios, claim, OTA | [Radios](https://docs.espressif.com/projects/esp-minihub/en/latest/radios.html) | Treat Thread as a device point |

Development guide: <https://docs.espressif.com/projects/esp-minihub/en/latest/>.
Latest news: <https://minihub.espressif.com/>.
To build the guide locally: [docs/README.md](docs/README.md).

## Why MiniHub

* **Host / plugin split** — Espressif stacks and hub services stay in Host;
  vendor clouds and OEM extras ship as `.mhub`
* **Point-centric** — UI, REST, rules, and scenes use `point_id`, not vendor APIs
* **OEM without Core fork** — product = `application/APP/` (board, partitions, seeds)
* **Field plugin upgrade** — install/upgrade Store modules without reflashing Host (ABI match)
* **Soft reload** — `POST /api/hub/reload` applies config and plugins without a full reboot

## Products

**Product name = folder under [`application/`](application/).** Shared Core:
[`components/`](components/).

| Product | Path | Summary |
|---------|------|---------|
| **minihub** | [`application/minihub/`](application/minihub/) | Generic Host (Web / Store / adapters). Ethernet-first; optional LCD in that app’s README. `-app minihub` / `MINIHUB_APP`. |

See [`application/README.md`](application/README.md).

## Requirements

* ESP-IDF 6.2, pinned to `release/v6.2` at `2b10d1278a547f5dc92efbe516144b4ae85f80cb`
  (`git describe` = `qa-test-full-v6.2-20261115`). The build reads `$IDF_PATH`,
  default `./esp-idf`
* Python 3.10+ and PyYAML
* Optional: `ESP_MATTER_PATH` when Matter is compiled in

## Quick start

```bash
./tools/build_firmware.py -app minihub build
./tools/build_firmware.py -app minihub -p /dev/ttyUSB0 app-flash monitor
```

Stacks: `zigbee` · `ble_mesh` · `matter` · `thread` (Thread is **BR only**, not
a device adapter). Details: [getting started](https://docs.espressif.com/projects/esp-minihub/en/latest/getting_started.html).

## Repository layout

| Path | Role |
|------|------|
| `application/` | Per-product firmware (folder name = product) |
| `components/` | Shared Core, Host adapters, Store runtime |
| `configs/` | Shared manifest base, adapter seeds, sdkconfig fragments |
| `examples/` | Standalone Store demos — one directory: [`plugins/`](examples/README.md) |
| `tools/` | Build, pack, Store server, CI |
| `docs/` | Programming Guide (RST / esp-docs) |
| `docs/module_sdk/` | L3 Host export allowlist |

## Data model (short)

Core is **type-agnostic**. One physical device = one `minihub_point_t`
(`point_id` / `type` string / summary `value` / `props` / `adapter` / `online`).
Matter is **one endpoint = one point** (`light/matter_n{node}_ep{ep}`). Thread
does not register device points.

* Live parameters: `props.params.<name>`. Identity (`manufacturer`, `category`,
  `owner`, `ota`, …) is **top-level** `props`, not catalog params.
* UI widgets come from the Host **type catalog** (CSV), protocol-agnostic.
* Plugins that own a device `point_register` once. Vendor claim / OTA on a Host
  radio point uses the **same** `point_id` and `merge_props` — never a second
  point. Commands still use `point.adapter` (the radio).
* Telemetry updates: `merge_params` / `merge_props`, not a full `props` replace.
* Rules (`command` / `command_by_type` / `delay` / `notify`): [rules](https://docs.espressif.com/projects/esp-minihub/en/latest/rules.html).
  Scenes are a separate REST path.

## Store plugins

| Tier | Package | Demo |
|------|---------|------|
| L1 | `module.json` + `inject` | [`ble_mesh_pack`](examples/plugins/ble_mesh_pack/) |
| L2 | `module.json` + `main.js` | [`color_bulb_js`](examples/plugins/color_bulb_js/), [`meter_plug_js`](examples/plugins/meter_plug_js/) |
| L3 | `module.json` + `.so` | none in-tree — scaffold with `tools/new_store_module.py --tier l3` |

Index: [examples/README.md](examples/README.md). Do not add demos to public
`store_preinstall`.

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack <id>
```

## Documentation

* [English index](https://docs.espressif.com/projects/esp-minihub/en/latest/index.html) · [中文索引](https://docs.espressif.com/projects/esp-minihub/zh_CN/latest/index.html)
* [Build HTML](docs/README.md)
* [Contributing](https://docs.espressif.com/projects/esp-minihub/en/latest/contributing.html) · [AGENTS.md](AGENTS.md)

## Contributing

[Contributing](https://docs.espressif.com/projects/esp-minihub/en/latest/contributing.html): conventional commits,
underscores in branch names (no hyphens), Host Core stays ecosystem-agnostic,
vendor logic belongs in Store plugins.

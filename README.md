# MiniHub

ESP32 Host firmware: Web UI, REST, Store dynamic modules (`.mhub`), and multi-protocol adapters (Zigbee / BLE Mesh / Matter / MQTT / Modbus).

Third-party ecosystems (e.g. MIoT) ship as **Store plugins**, not Host dependencies.

## Products (applications)

**Product name = folder under [`application/`](application/).** Shared Core lives in [`components/`](components/).

| Product | Path | Summary |
|---------|------|---------|
| **minihub** | [`application/minihub/`](application/minihub/) | Public Host (Web / Store / adapters). Default networking is Ethernet-oriented; optional LCD recipe in that app’s README. Select with `-app minihub` / `MINIHUB_APP`. |
| **claw_minihub** | `application/claw_minihub/` (optional) | Claw shell + MiniHub Core (local MCP) when that tree is present. |

See [`application/README.md`](application/README.md) and [application/minihub/README.md](application/minihub/README.md).

## Requirements

- ESP-IDF ~6.2 (ESP32-S31 / project CI image)
- Python 3.10+

## Quick start

```bash
./tools/build_firmware.py -app minihub zigbee build flash monitor
```

Radio stacks: `zigbee` · `ble_mesh` · `matter`.

## Layout

| Path | Role |
|------|------|
| `application/` | Per-product firmware (folder name = product) |
| `components/` | Shared Core, adapters, Store |
| `config/` | Shared manifest / adapters / sdkconfig fragments |
| `tools/` | Build, pack, Store, CI |
| `docs/` | Developer docs |

## Docs

- [Build](docs/build.md) · [Plugins & Store](docs/plugins_and_store.md) · [AGENTS.md](AGENTS.md)

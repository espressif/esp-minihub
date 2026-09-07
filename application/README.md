# MiniHub applications (products)

**Each `application/<name>/` directory is a product** (product name = folder name). Shared Core: [`components/`](../components/).

| Product | Path | Summary |
|---------|------|---------|
| **minihub** | [`minihub/`](minihub/) | Public Host. Ethernet-oriented by default; optional LCD — see [minihub/README.md](minihub/README.md). Select: `-app minihub`. |

```bash
./tools/build_firmware.py -app minihub zigbee build
```

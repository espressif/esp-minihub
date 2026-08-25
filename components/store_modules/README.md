# Store-installable modules

Place module sources as siblings under this directory (`components/store_modules/<id>/`).
This tree may be empty in the Host-only release; plugins are delivered as **`.mhub`** packages via Store.

| Tier | Typical contents | Deploy artifact |
|------|------------------|-----------------|
| L1 pack | `module.json` (+ inject) | `<id>.mhub` zip |
| L2 JS | `module.json` + `main.js` (+ `ui/` / `web/`) | `<id>.mhub` zip |
| L3 native | `module.json` + CMake / `.so` (+ UI) | MHMD `.so` + `<id>.mhub` zip |

Deploy directory (`MINIHUB_STORE_DEPLOY`, default `~/workspace/minihub_store`):

```text
<deploy>/catalog.json
<deploy>/<id>/<id>.mhub
```

```bash
./tools/build_firmware.py modules              # build + pack native modules (if sources present)
python3 tools/pack_store_module.py pack all        # → deploy/<id>/<id>.mhub (container: zip)
# python3 tools/pack_store_module.py mhmd <so> <out.so>   # native MHMD wrap only (not package format)
python3 tools/sync_store_catalog.py all
```

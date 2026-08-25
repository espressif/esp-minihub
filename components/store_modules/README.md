# Store-installable modules

Place module sources as siblings under this directory (`components/store_modules/<id>/`).
Public Host ships **`miot`** here as an L3 Store plugin (pack to `miot.mhub`); it is not a Host `require`.

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
./tools/build_firmware.py -app minihub build modules   # build + pack native modules
python3 tools/pack_store_module.py pack all             # → deploy/<id>/<id>.mhub
python3 tools/sync_store_catalog.py all
```

# Store-installable modules

Place module sources as siblings under this directory (`components/store_modules/<id>/`).
They ship as Store `.mhub`, not as Host `require`.

New modules (use templates, not an existing vendor plugin):

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native
```

Templates: `docs/module_sdk/templates/`. Agent skills: `.cursor/skills/minihub-plugin-l1` … `l3`.

Factory preinstall into device `/store/modules/<id>/` is **product-scoped**: list ids in `application/<app>/manifest.yml` as `store_preinstall: [<id>, …]`. The optional `"preinstall"` field in `module.json` is **not** used for factory bundling (kept for tooling/docs only).

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

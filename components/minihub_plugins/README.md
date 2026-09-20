# Store-installable modules

[English](README.md) | [中文](README_CN.md)

Module sources live as siblings here: `components/minihub_plugins/<id>/`.
Ship as Store `.mhub`, not Host `require`.

Standalone demos: `examples/plugins/<id>` (not symlinked here). Build with Host via
`MINIHUB_PLUGIN_DIRS` — see each demo README.

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native
```

Templates: `docs/module_sdk/templates/`. Skills: `.cursor/skills/minihub-plugin-l1` … `l3`.

Factory preinstall: `application/<app>/manifest.yml` → `store_preinstall: [<id>, …]`
into device `/store/modules/<id>/`. Ids resolve under `components/minihub_plugins/<id>/`
**or** a path in `MINIHUB_PLUGIN_DIRS`. (`module.json` `"preinstall"` is not used for
factory bundling.)

| Tier | Typical contents | Artifact |
|------|------------------|----------|
| L1 pack | `module.json` (+ inject) | `<id>.mhub` |
| L2 JS | `module.json` + `main.js` (+ UI) | `<id>.mhub` |
| L3 native | `module.json` + CMake / `.so` | MHMD `.so` + `<id>.mhub` |

Vendor plugins (Xiaomi MIoT) live in the sibling repo `esp-minihub-plugins`,
not under `examples/plugins`. Point `MINIHUB_PLUGIN_DIRS` at that clone.

Deploy (`MINIHUB_PLUGINS_DEPLOY_DIR`, default `~/workspace/minihub_store`):

```text
<deploy>/catalog.json
<deploy>/<id>/<id>.mhub
```

```bash
./tools/build_firmware.py -app minihub build     # firmware; packs .mhub on success
./tools/build_firmware.py -app minihub modules   # compile every native .so
./tools/build_firmware.py -app minihub pack      # pack only (existing .so)
python3 tools/pack_store_module.py pack all
python3 tools/sync_store_catalog.py all
```

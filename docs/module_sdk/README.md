# Module SDK (Store plugins)

| Path | Purpose |
|------|---------|
| `host_export_allowlist.json` | Host ELF symbols L3 `.so` may leave undefined |
| `templates/l1/` | L1 pack (`inject` into mqtt) |
| `templates/l2/` | L2 JS (`main.js`) |
| `templates/l3/` | L3 native (`minihub_module_entry`) |

```bash
python3 tools/new_store_module.py --tier l2 --id my_js --name "My JS"
python3 tools/pack_store_module.py pack my_js
python3 tools/sync_store_catalog.py all
```

Do not copy `components/store_modules/miot/` as a template. Agent workflow: `AGENTS.md` and `.cursor/skills/minihub-plugin-l*/`.

# MIoT plugin tools

Offline checks for Spec / sync heuristics (not used at firmware runtime).

```bash
python3 components/minihub_plugins/miot/tools/check_miot_profile_cases.py
```

| File | Role |
|------|------|
| `check_miot_profile_cases.py` | Assert `profile_for_model` + Spec slim parse |
| `miot_profile_cases.json` | Heuristic cases (`model` → type/siid/piid) |

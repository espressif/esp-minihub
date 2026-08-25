# Local component patches

`application/<app>/managed_components/` is gitignored and re-downloaded by the ESP-IDF
Component Manager. App git submodules (e.g. `application/minihub/components/esp-rainmaker`)
and optional sibling checkouts may also need MiniHub-specific fixes.

Local fixes live here as unified diffs and are re-applied with:

```bash
./tools/apply_component_patches.sh
```

Run after `idf.py update-dependencies`, `idf.py reconfigure`, deleting an app
`managed_components/`, or after refreshing a submodule (`git submodule update`).

Optional patch header:

```
Apply-Root: application/minihub/components/esp-rainmaker
```

When omitted, the patch root is the app project (`application/minihub` by default;
override with `MINIHUB_APP_DIR`). Patch paths still start with `managed_components/...`.

## Patches

| Patch | Component | Purpose |
|-------|-----------|---------|
| `esp-boost-0.6.0-isr-safe-syscalls.patch` | `espressif/esp-boost` 0.6.0 (`e5a6e838`) | Zigbee + Wi-Fi coexistence: avoid `std::mutex` in `__wrap_write` for normal fds (ISR-safe). |
| `esp-rainmaker-1.16.0-soft-sha-fallback.patch` | `esp-rainmaker` v1.16.0 (`afda50d`) | Soft SHA-256 when `CONFIG_MBEDTLS_HARDWARE_SHA` is off (GDMA hang workaround). |
| `esp-rainmaker-1.16.0-defer-chal-resp-self-claim.patch` | `esp-rainmaker` v1.16.0 (`afda50d`) | Defer Challenge Response init during Self Claim so on-network APP mapping can start after claim. |
| `esp-matter-chipoble-null-peer-write.patch` | `../esp-matter` CHIP nimble BLE | Null-check peer in `SendWriteRequest` after GAP disconnect (CHIPoBLE StandAloneAck race → Load access fault). |

Check whether patches are applied (exit 1 if any are missing):

```bash
./tools/apply_component_patches.sh --check
```

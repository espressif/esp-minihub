# Local component patches

`application/<app>/managed_components/` is gitignored and re-downloaded by the ESP-IDF
Component Manager. App git submodules (e.g. `application/minihub/components/esp-rainmaker`)
and optional sibling checkouts may also need MiniHub-specific fixes.

**Retained patches (R4):** one esp-rainmaker fix is kept on purpose — it is not
upstreamed and the product depends on it:

- `esp-rainmaker-1.16.0-defer-chal-resp-self-claim.patch` — defer the commissioning
  challenge-response / self-claim so MiniHub can own the fabric.

**Dropped (superseded by `esp_sha_compat.c`, 2026-10-02):** the two soft-SHA fallback
patches (`esp-rainmaker-1.16.0-soft-sha-fallback.patch` and its `-managed` variant) made
`esp_rmaker_node_auth.c` call `mbedtls_md` instead of `esp_sha()` when
`CONFIG_MBEDTLS_HARDWARE_SHA` is off. `components/minihub_rainmaker/esp_sha_compat.c`
now provides a software `esp_sha()` (PSA SHA-256/SHA-1) under `!CONFIG_MBEDTLS_HARDWARE_SHA`,
so rainmaker's upstream `esp_sha()` call links and works without patching it. Both patch
files were deleted.

> Build note: these were restored per R4 after having been dropped in `226e317`. Validate
> they still `git apply` against the current `esp-rainmaker` version at the next build; if
> a patch no longer applies, refresh it or drop it (the repo built clean without them).

**Dropped patches (R4, accepted risk — NOT restored):** the following three were
intentionally removed; recoverable from history (`git log --diff-filter=D -- patches/`)
if a symptom returns:

- `esp-boost-0.6.0-isr-safe-syscalls.patch` — ISR-safe syscalls. Drop accepted: no legacy
  product needs it; re-introduce only if an ISR(unsafe) call surfaces.
- `esp-elf-loader-1.3.3-s31-psram-text-exec.patch` — PSRAM exec text. Drop accepted:
  elf_loader 1.3.3+ already handles S31 I-cache (see note at bottom); the `MALLOC_CAP_EXEC`
  need under `CONFIG_SPIRAM_XIP_FROM_PSRAM` is **unverified** —〔未验证假设〕.
- `esp-matter-chipoble-null-peer-write.patch` — CHIPoBLE null-peer write. **Drop accepted
  but note: the null deref it fixed (GAP disconnect → StandAloneAck → Load access fault)
  still exists in upstream esp-matter (`BLEManagerImpl.cpp` peer_find → peer_svc_find_uuid
  deref); not reproduced on this branch's hardware, so dropped pending a real repro.**〔接受风险〕

## Adding a patch

1. Fix the component in place, then write a unified diff against it:

   ```bash
   cd application/minihub && git diff -- managed_components/<comp> > \
     ../../patches/<component>-<version>-<what>.patch
   ```

2. Name it `<component>-<version>-<short-what>.patch` and put it in this directory.
   Paths in the diff start at the apply root (`managed_components/...` by default).
3. If the target is not the app project, add a header line — resolved relative to the
   MiniHub repo root, so a sibling checkout works too:

   ```
   Apply-Root: application/minihub/components/esp-rainmaker
   ```

4. Include a distinctive added line. `apply_component_patches.sh` greps for one to
   decide whether the patch is already applied, so a lone `#if` makes it re-apply
   forever. A missing target file is skipped, not an error, so a patch for an
   optional component (esp-boost only builds with LCD/GUI) does not fail the build.

## Applying

```bash
./tools/apply_component_patches.sh
./tools/apply_component_patches.sh --check   # dry-run, exit 1 if a patch is needed
```

Run after `idf.py update-dependencies`, `idf.py reconfigure`, deleting an app
`managed_components/`, or after refreshing a submodule (`git submodule update`).
`build_firmware.py` also re-runs this after set-target / board-manager fetch so
managed component patches apply on fresh CI before the final `idf.py build`.
The app project defaults to `application/minihub`; override with `MINIHUB_APP_DIR`.

## Before you write one

Prefer upstreaming, or a version pin in the lockfile, over a local diff: a patch
silently stops applying when the component is re-downloaded at a new version.

**S31 PSRAM I-cache:** `elf_loader` **1.3.3+** already uses full `Cache_WriteBack_All` /
`Cache_Invalidate_All` on ESP32-S31 (ranged `*_Addr` APIs no-op on unaligned `seg_size`).
Do not re-introduce a 1.3.2-era `esp_elf_arch_flush(addr,size)` patch against current
sources. If an L3 `.so` whose text lives in PSRAM (`CONFIG_ELF_LOADER_LOAD_PSRAM` +
`CONFIG_SPIRAM_XIP_FROM_PSRAM`) faults with Instruction access fault MCAUSE=1, the
removed `esp-elf-loader-1.3.3-s31-psram-text-exec.patch` added `MALLOC_CAP_EXEC` to the
text allocation in `esp_elf_adapter.c` — check whether the memory-protection config
(`CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION`) already covers it before restoring it.

# Local component patches

`application/<app>/managed_components/` is gitignored and re-downloaded by the ESP-IDF
Component Manager. App git submodules (e.g. `application/minihub/components/esp-rainmaker`)
and optional sibling checkouts may also need MiniHub-specific fixes.

**This tree currently carries no patches.** The six that used to live here (esp-boost
ISR-safe syscalls, elf-loader PSRAM exec text, CHIPoBLE null peer write, and three
esp-rainmaker self-claim / Soft SHA fixes) were dropped; they are recoverable from
history if a symptom comes back — `git log --diff-filter=D -- patches/`.

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
managed Soft SHA (etc.) applies on fresh CI before the final `idf.py build`.
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

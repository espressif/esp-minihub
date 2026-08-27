# claw_minihub Port Implementation Plan

> **For agentic workers:** Implement tasks in order; commit after sanitize + docs.

**Goal:** Port Claw product into `application/claw_minihub` with MIoT as Store plugin only.

**Spec:** `docs/superpowers/specs/2026-08-26-claw-minihub-port-design.md`

## Tasks

1. Branch `feature/claw_minihub_port`; rsync MiniHub claw tree (exclude `build/`, `managed_components/`, `sdkconfig.old`, `components/esp-claw`).
2. `git submodule update --init application/claw_minihub/components/esp-claw`.
3. Sanitize CMake/fatfs/Kconfig; strip MIoT from `cap_account_bind`; keep `cmd_miot` as RPC lab.
4. Refresh app README/docs; add `config/sdkconfig/app.yml` if needed for compose.
5. Smoke: confirm paths exist for `build_firmware.py -app claw_minihub`; commit.

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""
Unified MiniHub radio-stack firmware build.

Usage:
  ./tools/build_firmware.py [-app NAME] [stacks...] [--] [idf.py args...]
  ./tools/build_firmware.py modules [all|module_id ...]
  ./tools/build_firmware.py merge

Apps (-app / --app; folder under application/):
  minihub         Public Host (default)
  claw_minihub    Claw + MCP product

  Product = application/<app>/ (MINIHUB_APP), e.g. minihub / claw_minihub.
  Sdkconfig recipe: application/<app>/config/sdkconfig/app.yml

Stacks (lowercase):
  ble_mesh | zigbee | matter | thread
  thread and zigbee are mutually exclusive (IEEE 802.15.4).

Defaults (no stack args):  zigbee ble_mesh

Examples:
  ./tools/build_firmware.py -app minihub zigbee build
  ./tools/build_firmware.py ble_mesh matter build
  ./tools/build_firmware.py thread matter build
  ./tools/build_firmware.py build merge
  ./tools/build_firmware.py modules
"""

from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

# Allow `from build_store_modules import …` when run as ./tools/build_firmware.py
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_store_modules import pack_modules  # noqa: E402
from compose_sdkconfig import (  # noqa: E402
    append_local_overrides,
    app_feature_list,
    app_has_feature,
    expand_features,
)

STACKS = frozenset({"ble_mesh", "zigbee", "matter", "thread"})
DEFAULT_STACKS = ("zigbee", "ble_mesh")
IDF_VERBS = frozenset(
    {
        "build",
        "flash",
        "app-flash",
        "monitor",
        "menuconfig",
        "fullclean",
        "clean",
        "reconfigure",
        "size",
        "size-components",
        "size-files",
        "erase-flash",
        "encrypted-app-flash",
        "uf2",
        "uf2-app",
        "modstore-flash",
    }
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_APP = "minihub"
GEN_STACKS = ROOT / "cmake" / "generated" / "sdkconfig_stacks.gen"
LOCAL_VERBS = frozenset({"merge"})

APP_NAME = DEFAULT_APP
APP_DIR = ROOT / "application" / DEFAULT_APP
BUILD_DIR = APP_DIR / "build"


def _sdkconfig_path_for_app(rel: str) -> str:
    """SDKCONFIG_DEFAULTS entries are resolved relative to the IDF project dir (APP_DIR)."""
    root_path = ROOT / rel
    app_local = APP_DIR / rel
    if app_local.is_file():
        return os.path.relpath(app_local, APP_DIR)
    if root_path.is_file():
        return os.path.relpath(root_path, APP_DIR)
    raise FileNotFoundError(f"SDKCONFIG_DEFAULTS fragment missing: {rel}")


def set_app(app_name: str) -> None:
    """Select application/<app_name> as the IDF project root."""
    global APP_NAME, APP_DIR, BUILD_DIR
    APP_NAME = app_name
    APP_DIR = ROOT / "application" / APP_NAME
    BUILD_DIR = APP_DIR / "build"


def list_apps() -> list[str]:
    apps_root = ROOT / "application"
    if not apps_root.is_dir():
        return []
    names: list[str] = []
    for p in sorted(apps_root.iterdir()):
        if p.is_dir() and (p / "CMakeLists.txt").is_file():
            names.append(p.name)
    return names


def resolve_app(cli_app: str | None) -> str:
    if cli_app:
        name = cli_app.strip()
    else:
        name = (os.environ.get("MINIHUB_APP") or DEFAULT_APP).strip() or DEFAULT_APP
    app_dir = ROOT / "application" / name
    if not (app_dir / "CMakeLists.txt").is_file():
        known = ", ".join(list_apps()) or "(none)"
        print(
            f"Unknown or incomplete app: {name!r} (expected folder under application/ "
            f"with CMakeLists.txt). Available: {known}",
            file=sys.stderr,
        )
        usage(2)
    return name



def usage(code: int = 0) -> None:
    print(
        """Usage:
  ./tools/build_firmware.py [-app NAME] [stacks...] [--] [idf.py args...]
  ./tools/build_firmware.py modules [all|module_id ...]
  ./tools/build_firmware.py merge

Apps: minihub (default) | claw_minihub
Product: -app / MINIHUB_APP (application/<name>/)
Stacks: ble_mesh | zigbee | matter | thread (default: zigbee ble_mesh)
thread and zigbee are mutually exclusive (IEEE 802.15.4).

Examples:
  ./tools/build_firmware.py -app minihub zigbee build
  ./tools/build_firmware.py build flash monitor
  ./tools/build_firmware.py build merge
  ./tools/build_firmware.py modules
""",
        file=sys.stderr if code else sys.stdout,
    )
    raise SystemExit(code)


def parse_argv(argv: list[str]) -> tuple[str, str, list[str], list[str]]:
    """Return (app, stacks, idf_args)."""
    if argv and argv[0] in {"-h", "--help", "help"}:
        usage(0)

    stacks: list[str] = []
    idf_args: list[str] = []
    seen_sep = False
    cli_app: str | None = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if not seen_sep and a in {"--board", "-B"}:
            print(
                "ERROR: --board was removed; use -app / MINIHUB_APP (product = application/<name>/).",
                file=sys.stderr,
            )
            usage(2)
        if not seen_sep and a.startswith("--board="):
            print(
                "ERROR: --board was removed; use -app / MINIHUB_APP (product = application/<name>/).",
                file=sys.stderr,
            )
            usage(2)
        if not seen_sep and a in {"-app", "--app"}:
            if i + 1 >= len(argv):
                print("-app requires an application/ folder name (e.g. minihub)", file=sys.stderr)
                usage(2)
            cli_app = argv[i + 1]
            i += 2
            continue
        if not seen_sep and a.startswith("--app="):
            cli_app = a.split("=", 1)[1]
            i += 1
            continue
        if not seen_sep and a.startswith("-app="):
            cli_app = a.split("=", 1)[1]
            i += 1
            continue
        if not seen_sep and a == "--":
            seen_sep = True
            i += 1
            continue
        if not seen_sep and a == "modules":
            app = resolve_app(cli_app)
            return app, [], ["__modules__", *argv[i + 1 :]]
        if not seen_sep and a in STACKS:
            if a not in stacks:
                stacks.append(a)
            i += 1
            continue
        seen_sep = True
        idf_args.append(a)
        i += 1

    if not idf_args:
        idf_args = ["build"]
    elif idf_args[0] not in IDF_VERBS and idf_args[0] not in LOCAL_VERBS and not idf_args[0].startswith("-"):
        if idf_args[0] not in STACKS:
            print(f"Unknown argument: {idf_args[0]!r} (stacks: {', '.join(sorted(STACKS))})", file=sys.stderr)
            usage(2)

    if not stacks:
        app = resolve_app(cli_app)
        stacks = [] if app == "claw_minihub" else list(DEFAULT_STACKS)
    else:
        app = resolve_app(cli_app)
    return app, stacks, idf_args


def split_local_verbs(idf_args: list[str]) -> tuple[list[str], list[str]]:
    """Pull MiniHub-local verbs out of the idf.py argument list (order preserved)."""
    local: list[str] = []
    rest: list[str] = []
    for a in idf_args:
        if a in LOCAL_VERBS:
            if a not in local:
                local.append(a)
        else:
            rest.append(a)
    return local, rest


def run_merge_bin(out_name: str = "minihub_merged.bin") -> int:
    """Merge partition images listed in build/flasher_args.json into one flashable bin."""
    args_path = BUILD_DIR / "flasher_args.json"
    if not args_path.is_file():
        print(
            f"ERROR: {args_path} missing — run a successful firmware build first.",
            file=sys.stderr,
        )
        return 1

    data = json.loads(args_path.read_text(encoding="utf-8"))
    flash_files = data.get("flash_files") or {}
    if not flash_files:
        print(f"ERROR: no flash_files in {args_path}", file=sys.stderr)
        return 1

    settings = data.get("flash_settings") or {}
    flash_mode = settings.get("flash_mode") or "dio"
    flash_freq = settings.get("flash_freq") or "80m"
    flash_size = settings.get("flash_size") or "16MB"
    chip = (data.get("extra_esptool_args") or {}).get("chip") or "esp32s31"

    out_path = BUILD_DIR / out_name
    cmd: list[str] = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        str(chip),
        "merge-bin",
        "-o",
        str(out_path),
        "--flash-mode",
        str(flash_mode),
        "--flash-freq",
        str(flash_freq),
        "--flash-size",
        str(flash_size),
    ]

    missing: list[str] = []
    for offset, rel in sorted(flash_files.items(), key=lambda kv: int(str(kv[0]), 0)):
        path = BUILD_DIR / rel
        if not path.is_file():
            missing.append(f"{offset} -> {rel}")
            continue
        off = offset if str(offset).startswith("0x") else hex(int(str(offset), 0))
        cmd.extend([off, str(path)])

    if missing:
        print("ERROR: missing flash input(s):", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        return 1

    print("=== merge-bin ===", flush=True)
    print(f"chip={chip} mode={flash_mode} freq={flash_freq} size={flash_size}", flush=True)
    print(f"output={out_path}", flush=True)
    for offset, rel in sorted(flash_files.items(), key=lambda kv: int(str(kv[0]), 0)):
        print(f"  {offset}  {rel}", flush=True)
    print(flush=True)
    # Prefer IDF-exported esptool; fall back to plain python -m esptool.
    idf_path = Path(os.environ.get("IDF_PATH", str(ROOT / "esp-idf")))
    export_sh = idf_path / "export.sh"
    if export_sh.is_file():
        script = "\n".join(
            [
                "set -euo pipefail",
                f'export IDF_PATH="{idf_path}"',
                'source "$IDF_PATH/export.sh" >/dev/null',
                " ".join(shlex.quote(c) for c in cmd),
            ]
        )
        rc = subprocess.run(["bash", "-c", script], cwd=str(ROOT)).returncode
    else:
        rc = subprocess.run(cmd, cwd=str(ROOT)).returncode

    if rc == 0 and out_path.is_file():
        sz = out_path.stat().st_size
        print(f"Wrote {out_path} ({sz} bytes, {sz / (1024 * 1024):.2f} MiB)")
        print(f"Flash: esptool.py --chip {chip} write_flash 0x0 {out_path}")
    return rc


def maybe_pack_modules_after_build(idf_args: list[str], build_rc: int) -> int:
    """After successful firmware build, pack all binary store modules."""
    if build_rc != 0:
        return build_rc
    if "build" not in idf_args:
        return build_rc
    if os.environ.get("MINIHUB_SKIP_MHUB", "").strip() in {"1", "true", "yes"}:
        print("MINIHUB_SKIP_MHUB set — skipping store module .mhub pack")
        return build_rc
    print()
    print("=== Packing store modules (.mhub) ===")
    try:
        return pack_modules(["all"])
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        return exc.returncode or 1


def validate_stacks(stacks: list[str]) -> None:
    unknown = [s for s in stacks if s not in STACKS]
    if unknown:
        print(f"Unknown stack(s): {', '.join(unknown)}", file=sys.stderr)
        usage(2)
    if "thread" in stacks and "zigbee" in stacks:
        print(
            "ERROR: stacks 'thread' and 'zigbee' both use IEEE 802.15.4 — pick one "
            "(omit zigbee on the CLI, or clear CONFIG_MINIHUB_THREAD_ADAPTER / "
            "CONFIG_MINIHUB_ZIGBEE_SDK in sdkconfig).",
            file=sys.stderr,
        )
        sys.exit(2)


def manifest_app_name() -> str:
    """Manifest / sync key is the product (application folder)."""
    return APP_NAME


def write_stacks_gen(stacks: list[str]) -> None:
    """Pin radio-stack Kconfig on/off (last wins among SDKCONFIG_DEFAULTS)."""
    GEN_STACKS.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated by tools/build_firmware.py — do not edit",
        f"# stacks: {' '.join(stacks)}",
        "",
    ]
    if "ble_mesh" in stacks:
        lines.append("CONFIG_MINIHUB_BLE_MESH_SDK=y")
    else:
        lines.append("# CONFIG_MINIHUB_BLE_MESH_SDK is not set")
        lines.append("# CONFIG_BLE_MESH is not set")
    if "zigbee" in stacks:
        lines.append("CONFIG_MINIHUB_ZIGBEE_SDK=y")
    else:
        lines.append("# CONFIG_MINIHUB_ZIGBEE_SDK is not set")
        lines.append("# CONFIG_ZB_ENABLED is not set")
    if "matter" in stacks:
        lines.append("CONFIG_MINIHUB_MATTER_ADAPTER=y")
    else:
        lines.append("# CONFIG_MINIHUB_MATTER_ADAPTER is not set")
    if "thread" in stacks:
        lines.append("CONFIG_MINIHUB_THREAD_ADAPTER=y")
    else:
        lines.append("# CONFIG_MINIHUB_THREAD_ADAPTER is not set")
        lines.append("# CONFIG_OPENTHREAD_ENABLED is not set")
    lines.append("")
    GEN_STACKS.write_text("\n".join(lines), encoding="utf-8")


def sync_stack_symbols_in_sdkconfig(stacks: list[str]) -> None:
    """Ensure selected stacks are =y in application sdkconfig.

    Does not force unselected stacks to 'is not set' — that undoes manual
    menuconfig / hand-edits. Disabled stacks are listed in stacks.gen only;
    Matter/Thread also use ensure_*_stack when sdkconfig already has =y.
    """
    path = APP_DIR / "sdkconfig"
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    specs = [
        ("CONFIG_MINIHUB_BLE_MESH_SDK", "ble_mesh" in stacks),
        ("CONFIG_MINIHUB_ZIGBEE_SDK", "zigbee" in stacks),
        ("CONFIG_MINIHUB_MATTER_ADAPTER", "matter" in stacks),
        ("CONFIG_MINIHUB_THREAD_ADAPTER", "thread" in stacks),
    ]

    changed = False
    for sym, enabled in specs:
        if not enabled:
            continue
        on = f"{sym}=y"
        new_text, n = re.subn(rf"(?m)^(?:#\s*)?{re.escape(sym)}(?:=y|\s+is not set)\s*$", on, text)
        if n == 0 and on not in text:
            text = text.rstrip() + "\n" + on + "\n"
            changed = True
        elif n:
            text = new_text
            changed = True
    if changed:
        path.write_text(text, encoding="utf-8")


def sync_claw_wifi_bss_in_sdkconfig() -> None:
    """Keep net80211/Wi-Fi .bss in internal DRAM (Claw S31 + FAT before Wi-Fi init)."""
    if APP_NAME != "claw_minihub":
        return
    path = APP_DIR / "sdkconfig"
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    patches = [
        (r"(?m)^CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=.*$", "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=n"),
        (r"(?m)^CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=.*$", "CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=n"),
        (r"(?m)^CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY=.*$", "# CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set"),
    ]
    changed = False
    for pattern, replacement in patches:
        new_text, n = re.subn(pattern, replacement, text)
        if n:
            text = new_text
            changed = True
    if changed:
        path.write_text(text, encoding="utf-8")


def sync_radio_memory_in_sdkconfig(stacks: list[str]) -> None:
    """Ensure radio+Wi-Fi builds keep enough internal DRAM (see radio_coexist_memory.defaults)."""
    if not any(s in stacks for s in ("thread", "zigbee", "ble_mesh")):
        return
    path = APP_DIR / "sdkconfig"
    if not path.is_file():
        return
    target = "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=24576"
    text = path.read_text(encoding="utf-8", errors="replace")
    new_text, n = re.subn(
        r"(?m)^CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=\d+\s*$",
        target,
        text,
    )
    if n:
        path.write_text(new_text, encoding="utf-8")
    elif target not in text:
        path.write_text(text.rstrip() + "\n" + target + "\n", encoding="utf-8")


def ensure_git_submodule(rel_path: str) -> Path:
    """Init/update one submodule by path (does not recurse into others)."""
    dest = ROOT / rel_path
    marker = dest / "components"
    if marker.is_dir():
        return dest
    print(f"git submodule update --init {rel_path}")
    subprocess.run(
        ["git", "-C", str(ROOT), "submodule", "update", "--init", "--depth", "1", rel_path],
        check=True,
    )
    if not marker.is_dir():
        raise FileNotFoundError(
            f"submodule {rel_path} missing after init; "
            f"run: git submodule update --init {rel_path}"
        )
    return dest


def rainmaker_managed_dir() -> Path:
    return ROOT / "application" / "minihub" / "components" / "esp-rainmaker"


def rainmaker_components_dir() -> Path:
    """Prefer app submodule; fall back to sibling ../esp-rainmaker."""
    managed = rainmaker_managed_dir() / "components"
    if managed.is_dir():
        return managed.resolve()
    return (ROOT.parent / "esp-rainmaker" / "components").resolve()


def ensure_esp_rainmaker() -> None:
    """Ensure application/minihub/components/esp-rainmaker submodule is checked out."""
    ensure_git_submodule("application/minihub/components/esp-rainmaker")


def rainmaker_build_enabled() -> bool:
    """Whether to link full esp-rainmaker (submodule when enabled).

    Force with MINIHUB_BUILD_RAINMAKER=1|0. Default for minihub: follow
    application/minihub/config/sdkconfig/feature/rainmaker.defaults.
    """
    if APP_NAME != "minihub":
        return os.environ.get("MINIHUB_BUILD_RAINMAKER") == "1"
    env = os.environ.get("MINIHUB_BUILD_RAINMAKER")
    if env is not None:
        return env.strip().lower() in {"1", "true", "yes"}
    defaults = (
        ROOT
        / "application"
        / "minihub"
        / "config"
        / "sdkconfig"
        / "feature"
        / "rainmaker.defaults"
    )
    if not defaults.is_file():
        return False
    text = defaults.read_text(encoding="utf-8")
    return any(
        line.strip() == "CONFIG_MINIHUB_RAINMAKER=y"
        for line in text.splitlines()
    )


def sdkconfig_has(option_y: str) -> bool:
    """True if application sdkconfig sets CONFIG_FOO=y."""
    path = APP_DIR / "sdkconfig"
    if not path.is_file():
        return False
    needle = f"{option_y}=y"
    return any(
        line.strip() == needle
        for line in path.read_text(encoding="utf-8", errors="ignore").splitlines()
    )


def sdkconfig_disabled(option: str) -> bool:
    """True if sdkconfig explicitly disables option (=n or '# CONFIG_FOO is not set')."""
    path = APP_DIR / "sdkconfig"
    if not path.is_file():
        return False
    off_n = f"{option}=n"
    off_hash = f"# {option} is not set"
    return any(
        line.strip() in {off_n, off_hash}
        for line in path.read_text(encoding="utf-8", errors="ignore").splitlines()
    )


_STACK_KCONFIG = {
    "ble_mesh": "CONFIG_MINIHUB_BLE_MESH_SDK",
    "zigbee": "CONFIG_MINIHUB_ZIGBEE_SDK",
    "matter": "CONFIG_MINIHUB_MATTER_ADAPTER",
    "thread": "CONFIG_MINIHUB_THREAD_ADAPTER",
}


def drop_sdkconfig_disabled_stacks(stacks: list[str]) -> list[str]:
    """Remove CLI/default stacks that sdkconfig has turned off (e.g. ZIGBEE=n)."""
    out: list[str] = []
    for s in stacks:
        sym = _STACK_KCONFIG.get(s)
        if sym and sdkconfig_disabled(sym):
            print(
                f"{sym} disabled in sdkconfig → dropping stack '{s}'",
                flush=True,
            )
            continue
        out.append(s)
    return out


def ensure_matter_stack(stacks: list[str]) -> list[str]:
    """If MATTER_ADAPTER=y in sdkconfig, pull in the matter stack automatically."""
    if "matter" in stacks:
        return stacks
    if not sdkconfig_has("CONFIG_MINIHUB_MATTER_ADAPTER"):
        return stacks
    out = list(stacks)
    out.append("matter")
    print(
        "CONFIG_MINIHUB_MATTER_ADAPTER=y → adding stack 'matter' "
        "(clear the Kconfig option to build without ESP-Matter)",
        flush=True,
    )
    return out


def ensure_thread_stack(stacks: list[str]) -> list[str]:
    """If THREAD_ADAPTER=y in sdkconfig, pull in the thread stack automatically."""
    if "thread" in stacks:
        return stacks
    if not sdkconfig_has("CONFIG_MINIHUB_THREAD_ADAPTER"):
        return stacks
    out = list(stacks)
    out.append("thread")
    print(
        "CONFIG_MINIHUB_THREAD_ADAPTER=y → adding stack 'thread' "
        "(clear the Kconfig option to build without OpenThread BR)",
        flush=True,
    )
    return out


def sdkconfig_defaults(stacks: list[str]) -> str:
    # Do not list board_manager.defaults here. esp_board_manager 0.5+ injects it.
    if APP_NAME == "claw_minihub":
        base = [
            "sdkconfig.defaults",
            "sdkconfig.defaults.minihub",
            "sdkconfig.defaults.claw_board",
        ]
        # Optional shared eth fragments if this app defines the profile
        feats: list[str] = []
        try:
            feats, _board = app_feature_list(ROOT, APP_NAME)
            base.extend(expand_features(ROOT, APP_NAME, feats))
        except FileNotFoundError:
            pass
        if "zigbee" in stacks:
            base.append("config/sdkconfig/feature/zigbee.defaults")
        if "ble_mesh" in stacks:
            base.append("config/sdkconfig/feature/ble_mesh.defaults")
        if "thread" in stacks:
            base.append("config/sdkconfig/feature/thread.defaults")
        if "matter" in stacks:
            base.extend(
                [
                    "config/sdkconfig/feature/matter.defaults",
                    "config/sdkconfig/feature/matter_memory.defaults",
                    "config/sdkconfig/feature/matter_ble.defaults",
                ]
            )
            if app_has_feature(ROOT, APP_NAME, "eth") or "eth" in feats:
                base.append("config/sdkconfig/feature/matter_over_eth.defaults")
        # After matter_memory (16384 reserve): radio coexist wins for Thread/BT+Wi-Fi headroom.
        if "zigbee" in stacks or "ble_mesh" in stacks or "thread" in stacks:
            base.append("config/sdkconfig/feature/radio_coexist_memory.defaults")
        # After shared memory.defaults (BSS external=y): Claw Wi-Fi safety wins.
        base.append("config/sdkconfig/claw_wifi_bss.defaults")
        base.append("cmake/generated/sdkconfig_stacks.gen")
        paths = append_local_overrides(ROOT, APP_NAME, base)
        return ";".join(_sdkconfig_path_for_app(p) for p in paths)

    features, _board = app_feature_list(ROOT, APP_NAME)
    if rainmaker_build_enabled() and "rainmaker" not in features:
        features = list(features) + ["rainmaker"]
    if "zigbee" in stacks:
        features.append("zigbee")
    if "ble_mesh" in stacks:
        features.append("ble_mesh")
    if "thread" in stacks:
        features.append("thread")
    if "matter" in stacks:
        features.extend(["matter", "matter_memory", "matter_ble"])
        if app_has_feature(ROOT, APP_NAME, "eth") or "eth" in features:
            features.append("matter_over_eth")
    # After matter_memory: radio coexist reserve (24576) must not be overwritten.
    if "zigbee" in stacks or "ble_mesh" in stacks or "thread" in stacks:
        features.append("radio_coexist_memory")
    features.append("stacks.gen")
    paths = append_local_overrides(ROOT, APP_NAME, expand_features(ROOT, APP_NAME, features))
    return ";".join(_sdkconfig_path_for_app(p) for p in paths)


def matter_env_exports() -> list[str]:
    """Shell lines to export Matter paths / bootstrap esp-matter."""
    default_matter = ROOT.parent / "esp-idf/mesh_lite_v2_250925/examples/matter_bridge/root/esp-matter"
    matter = Path(os.environ.get("ESP_MATTER_PATH", str(default_matter)))
    lines = [
        f'export ESP_MATTER_PATH="{matter}"',
        "export MINIHUB_BUILD_MATTER=1",
        'if [[ ! -d "$ESP_MATTER_PATH" ]]; then',
        '  echo "ESP_MATTER_PATH not found: $ESP_MATTER_PATH" >&2',
        "  exit 1",
        "fi",
        'source "$ESP_MATTER_PATH/export.sh"',
        'GN_BIN="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/cipd/packages/pigweed/gn"',
        'CHIP_ROOT="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip"',
        'if [[ ! -x "$GN_BIN" ]]; then',
        '  echo "Bootstrapping esp-matter CHIP tools (first time, ~10 min)..."',
        '  if [[ ! -f "$CHIP_ROOT/third_party/pigweed/repo/pw_env_setup/util.sh" ]]; then',
        '    (cd "$CHIP_ROOT" && git submodule update --init third_party/pigweed/repo)',
        "  fi",
        '  if [[ ! -d "$CHIP_ROOT/third_party/uriparser/repo/include" ]]; then',
        '    (cd "$CHIP_ROOT" && git submodule update --init third_party/uriparser/repo)',
        "  fi",
        '  bash "$CHIP_ROOT/scripts/setup/bootstrap.sh"',
        '  python3 "$CHIP_ROOT/scripts/checkout_submodules.py" --shallow --platform esp32',
        '  source "$ESP_MATTER_PATH/export.sh"',
        "fi",
        'if [[ ! -x "$GN_BIN" ]]; then',
        '  echo "gn still missing after bootstrap" >&2',
        "  exit 1",
        "fi",
    ]
    return lines


def board_manager_board_name() -> str:
    _features, board = app_feature_list(ROOT, APP_NAME)
    return board


def needs_esp32s31_set_target() -> bool:
    """True when sdkconfig is missing or not targeting esp32s31 (fresh CI checkout)."""
    sdkconfig = APP_DIR / "sdkconfig"
    if not sdkconfig.is_file():
        return True
    try:
        text = sdkconfig.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return True
    return 'CONFIG_IDF_TARGET="esp32s31"' not in text


def gen_bmgr_codes_dir() -> Path:
    """Directory that holds gen_bmgr_codes (follows app symlink into repo components/)."""
    link = APP_DIR / "components" / "gen_bmgr_codes"
    if link.is_symlink() or not link.exists():
        # Symlink may point at a not-yet-created tree (CI fresh checkout).
        raw = os.readlink(link) if link.is_symlink() else None
        if raw:
            target = (link.parent / raw).resolve()
            target.mkdir(parents=True, exist_ok=True)
            return target
    link.mkdir(parents=True, exist_ok=True)
    return link.resolve() if link.exists() else link


def ensure_gen_bmgr_placeholder() -> None:
    """Create a minimal gen_bmgr_codes component so set-target/cmake can resolve REQUIRES.

    Fresh CI: minihub only has a symlink to gitignored components/gen_bmgr_codes/;
    claw gitignores its local copy. Real codes are generated after Component Manager
    fetches esp_board_manager (see shell_bmgr_codegen).
    """
    dest = gen_bmgr_codes_dir()
    meta = dest / "gen_board_metadata.yaml"
    cmake = dest / "CMakeLists.txt"
    if meta.is_file() and cmake.is_file():
        return
    if cmake.is_file() and not meta.is_file():
        # Already a placeholder or partial tree; leave for codegen.
        return
    (dest / "idf_component.yml").write_text("dependencies: {}\n", encoding="utf-8")
    (dest / "bmgr_placeholder.c").write_text(
        "/* Placeholder until esp_board_manager codegen runs. */\n",
        encoding="utf-8",
    )
    cmake.write_text(
        'idf_component_register(SRCS "bmgr_placeholder.c" INCLUDE_DIRS ".")\n',
        encoding="utf-8",
    )
    print(f"gen_bmgr_codes: placeholder at {dest} (will regenerate after deps fetch)")


def ensure_board_manager() -> None:
    """Regenerate <app>/components/gen_bmgr_codes for the profile hardware board."""
    bmgr_name = board_manager_board_name()
    meta = APP_DIR / "components/gen_bmgr_codes/gen_board_metadata.yaml"
    defaults = APP_DIR / "components/gen_bmgr_codes/board_manager.defaults"
    boards_dir = APP_DIR / "boards"
    current = ""
    if meta.is_file():
        for line in meta.read_text(encoding="utf-8").splitlines():
            if line.startswith("board:"):
                current = line.split(":", 1)[1].strip()
                break
    # claw_minihub (minihub_v2 style): local boards/ carries CONFIG_SPIRAM / FLASHSIZE.
    # Without -c ./boards, bmgr uses the managed catalog board (codec-only) and PSRAM stays off.
    defaults_text = defaults.read_text(encoding="utf-8") if defaults.is_file() else ""
    local_boards_stale = boards_dir.is_dir() and "CONFIG_SPIRAM=y" not in defaults_text
    # Regenerated under another checkout (e.g. MiniHub path baked into CMakeLists).
    cmake_txt = (
        (APP_DIR / "components/gen_bmgr_codes/CMakeLists.txt").read_text(encoding="utf-8")
        if (APP_DIR / "components/gen_bmgr_codes/CMakeLists.txt").is_file()
        else ""
    )
    board_path_stale = bool(cmake_txt) and str(APP_DIR.resolve()) not in cmake_txt
    if current == bmgr_name and defaults.is_file() and not local_boards_stale and not board_path_stale:
        return
    gen = APP_DIR / "managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py"
    if not gen.is_file():
        print(f"WARN: board manager script missing; will retry after idf deps fetch ({bmgr_name})", file=sys.stderr)
        return
    cmd = [sys.executable, str(gen), "-b", bmgr_name, "--project-dir", str(APP_DIR)]
    if boards_dir.is_dir():
        cmd.extend(["-c", str(boards_dir)])
    print(f"Switching board manager: {current or '(none)'} -> {bmgr_name}")
    if boards_dir.is_dir():
        print(f"  customer boards: {boards_dir}")
    subprocess.run(cmd, cwd=str(APP_DIR), check=True)


def shell_bmgr_codegen() -> list[str]:
    """Bash steps: generate gen_bmgr_codes once esp_board_manager is on disk."""
    bmgr_name = board_manager_board_name()
    meta = APP_DIR / "components/gen_bmgr_codes/gen_board_metadata.yaml"
    gen = APP_DIR / "managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py"
    boards_dir = APP_DIR / "boards"
    py_cmd = (
        f'python3 "$GEN" -b {shlex.quote(bmgr_name)} '
        f"--project-dir {shlex.quote(str(APP_DIR))}"
    )
    if boards_dir.is_dir():
        py_cmd += f" -c {shlex.quote(str(boards_dir))}"
    idf_bmgr = "idf.py --preview bmgr"
    if boards_dir.is_dir():
        idf_bmgr += " -c ./boards"
    idf_bmgr += f" -b {shlex.quote(bmgr_name)}"
    return [
        f"META={shlex.quote(str(meta))}",
        f"GEN={shlex.quote(str(gen))}",
        'if [ ! -f "$META" ]; then',
        '  echo "Generating components/gen_bmgr_codes (missing on fresh checkout)"',
        '  if [ ! -f "$GEN" ]; then',
        '    echo "esp_board_manager not fetched yet; running idf.py reconfigure"',
        "    idf.py --preview reconfigure",
        "  fi",
        '  if [ -f "$GEN" ]; then',
        f"    {py_cmd}",
        "  else",
        f"    {idf_bmgr}",
        "  fi",
        '  test -f "$META"',
        "fi",
    ]


def run_build(stacks: list[str], idf_args: list[str]) -> int:
    local_verbs, idf_args = split_local_verbs(idf_args)

    # merge-only: no idf.py invocation
    if not idf_args:
        if "merge" in local_verbs:
            return run_merge_bin()
        print("ERROR: no idf.py action (e.g. build / flash) and no local verb", file=sys.stderr)
        usage(2)

    stacks = drop_sdkconfig_disabled_stacks(stacks)
    stacks = ensure_matter_stack(stacks)
    stacks = ensure_thread_stack(stacks)
    validate_stacks(stacks)
    write_stacks_gen(stacks)
    sync_stack_symbols_in_sdkconfig(stacks)
    sync_radio_memory_in_sdkconfig(stacks)
    sync_claw_wifi_bss_in_sdkconfig()
    defaults = sdkconfig_defaults(stacks)
    idf_path = Path(os.environ.get("IDF_PATH", str(ROOT / "esp-idf")))
    espbaud = os.environ.get("ESPBAUD", "2000000")
    use_matter = "matter" in stacks
    use_rainmaker = rainmaker_build_enabled()

    print(f"product={APP_NAME} ({APP_DIR})")
    print(f"stacks={','.join(stacks)}")
    print(f"MINIHUB_APP={APP_NAME}")
    print(f"MINIHUB_BUILD_MATTER={1 if use_matter else 0}")
    print(f"MINIHUB_BUILD_RAINMAKER={1 if use_rainmaker else 0}")
    print(f"SDKCONFIG_DEFAULTS={defaults}")
    print(f"ESPBAUD={espbaud}")
    print(f"idf.py --preview {' '.join(idf_args)}")
    if local_verbs:
        print(f"local={' '.join(local_verbs)}")
    print()

    if not (APP_DIR / "CMakeLists.txt").is_file():
        print(f"ERROR: application project missing: {APP_DIR}", file=sys.stderr)
        return 1

    # sync_host_manifest before idf (needs no IDF env)
    subprocess.run(
        [sys.executable, str(ROOT / "tools/sync_host_manifest.py"), "--app", APP_NAME],
        cwd=str(ROOT),
        env={**os.environ, "MINIHUB_APP": APP_NAME},
        check=True,
    )
    # App git submodules (path-scoped; claw build never inits esp-rainmaker).
    if APP_NAME == "claw_minihub":
        ensure_git_submodule("application/claw_minihub/components/esp-claw")
    if use_rainmaker:
        ensure_esp_rainmaker()
    # Re-apply local patches to managed_components/ (gitignored, re-downloaded by IDF CM).
    subprocess.run(
        [str(ROOT / "tools/apply_component_patches.sh")],
        cwd=str(ROOT),
        env={**os.environ, "MINIHUB_APP_DIR": str(APP_DIR)},
        check=True,
    )
    ensure_board_manager()
    ensure_gen_bmgr_placeholder()

    idf_cmd = "idf.py --preview " + " ".join(shlex.quote(a) for a in idf_args)
    shell_parts = [
        "set -euo pipefail",
        f'cd "{APP_DIR}"',
        f'export IDF_PATH="{idf_path}"',
        f'export MINIHUB_APP="{APP_NAME}"',
        'export ESP_BMGR_ASSIST_PREFLIGHT=${ESP_BMGR_ASSIST_PREFLIGHT:-0}',
        f'export SDKCONFIG_DEFAULTS="{defaults}"',
        f'export ESPBAUD="{espbaud}"',
        f'export MINIHUB_BUILD_MATTER={"1" if use_matter else "0"}',
        f'export MINIHUB_BUILD_RAINMAKER={"1" if use_rainmaker else "0"}',
        # Mitigate intermittent GitHub pack failures (HTTP/2 / early EOF) in CI
        'git config --global http.version HTTP/1.1 || true',
        'git config --global http.postBuffer 524288000 || true',
        'git config --global http.lowSpeedLimit 1000 || true',
        'git config --global http.lowSpeedTime 600 || true',
        'source "$IDF_PATH/export.sh"',
    ]
    if use_matter:
        shell_parts.extend(matter_env_exports())
    else:
        shell_parts.append("export MINIHUB_BUILD_MATTER=0")
    # Fresh trees (CI) default to esp32; MiniHub boards need preview target esp32s31.
    if needs_esp32s31_set_target() and "set-target" not in idf_args:
        print("idf.py --preview set-target esp32s31 (sdkconfig missing or wrong target)")
        shell_parts.append("idf.py --preview set-target esp32s31")
    shell_parts.append("export IDF_TARGET=esp32s31")
    # gen_bmgr_codes is gitignored / symlink-to-ignored — generate after CM fetch
    shell_parts.extend(shell_bmgr_codegen())
    shell_parts.append(idf_cmd)

    script = "\n".join(shell_parts) + "\n"
    rc = subprocess.run(["bash", "-c", script], cwd=str(APP_DIR)).returncode
    rc = maybe_pack_modules_after_build(idf_args, rc)
    if rc == 0 and "merge" in local_verbs:
        print()
        rc = run_merge_bin()
    return rc


def main() -> None:
    app, stacks, idf_args = parse_argv(sys.argv[1:])
    set_app(app)
    os.environ["MINIHUB_APP"] = APP_NAME
    if idf_args and idf_args[0] == "__modules__":
        raw = idf_args[1:] if len(idf_args) > 1 else []
        deploy = None
        ids: list[str] = []
        i = 0
        while i < len(raw):
            if raw[i] in ("--deploy", "-d") and i + 1 < len(raw):
                deploy = Path(raw[i + 1])
                i += 2
                continue
            ids.append(raw[i])
            i += 1
        try:
            raise SystemExit(pack_modules(ids if ids else ["all"], deploy=deploy))
        except FileNotFoundError as exc:
            print(str(exc), file=sys.stderr)
            raise SystemExit(1)
        except subprocess.CalledProcessError as exc:
            raise SystemExit(exc.returncode or 1)
    raise SystemExit(run_build(stacks, idf_args))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""
Unified MiniHub radio-stack firmware build.

Usage:
  ./tools/build_firmware.py [-app NAME] [stacks...] [--release] [--] [idf.py args...]
  ./tools/build_firmware.py module [all|<id> ...]
  ./tools/build_firmware.py modules
  ./tools/build_firmware.py pack [all|<id> ...]
  ./tools/build_firmware.py merge
  ./tools/build_firmware.py lint [-j N] [<file> ...]   (clang-tidy over the IDF build)

Apps (-app / --app; folder under application/):
  minihub         Generic Host (default; Ethernet-oriented recipe)
  minihub_lcd     LCD-only Host (EMAC/RGB LCD pin groups are mutually exclusive)

  Product = application/<app>/ (MINIHUB_APP), e.g. minihub.
  -app / --app may appear anywhere before a literal `--`, including after
  idf.py options such as -p PORT. MINIHUB_APP=<name> works too.
  Sdkconfig recipe: application/<app>/configs/sdkconfig/app.yml

Stacks (lowercase, optional):
  ble_mesh | zigbee | matter | thread
  thread and zigbee may both be linked. If adapters.json enables both,
  the one that starts later is skipped (Zigbee is registered first).
  Omit stacks to use defaults (zigbee ble_mesh), then reconcile with the
  product sdkconfig: drop stacks that are disabled there; pull in matter /
  thread when those Kconfig options are already =y. Pass a stack name to
  force-enable it when it is not explicitly disabled in sdkconfig.

Flags:
  --release   Production log recipe (Warning). Loads
              configs/sdkconfig/feature/release.defaults and rewrites the
              saved application sdkconfig (Kconfig select alone does not).
              Same effect as adding `release` to app.yml features.

Examples:
  ./tools/build_firmware.py -app minihub build
  ./tools/build_firmware.py -app minihub_lcd --release build
  ./tools/build_firmware.py -p /dev/cu.usbserial-10 -app minihub_lcd flash monitor
  ./tools/build_firmware.py build flash monitor
  ./tools/build_firmware.py build merge
  ./tools/build_firmware.py vibe
  ./tools/build_firmware.py module my_native
  ./tools/build_firmware.py modules
  ./tools/build_firmware.py pack my_native
"""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Allow `from build_store_modules import …` when run as ./tools/build_firmware.py
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_store_modules import pack_modules, resolve_module_ids  # noqa: E402
from compose_sdkconfig import (  # noqa: E402
    append_local_overrides,
    app_feature_list,
    app_has_feature,
    expand_features,
    feature_layers,
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
        "storage-flash",
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
  ./tools/build_firmware.py [-app NAME] [stacks...] [--release] [--] [idf.py args...]
  ./tools/build_firmware.py module [all|<id> ...]
  ./tools/build_firmware.py modules
  ./tools/build_firmware.py pack [all|<id> ...]
  ./tools/build_firmware.py merge
  ./tools/build_firmware.py lint [-j N] [<file> ...]   (clang-tidy over the IDF build)

Apps: minihub (default) | minihub_lcd (LCD-only)
Product: -app / MINIHUB_APP (application/<name>/); -app works after -p PORT,
         everywhere before a literal `--`.
Stacks (optional): ble_mesh | zigbee | matter | thread
  default if omitted: zigbee ble_mesh; then reconcile with product sdkconfig
  (drop disabled; add matter/thread when already =y).
thread and zigbee may both be linked; adapters.json decides which runs.
--release: Warning-level logs (feature/release.defaults + rewrite sdkconfig).

Examples:
  ./tools/build_firmware.py -app minihub build
  ./tools/build_firmware.py -app minihub_lcd --release build
  ./tools/build_firmware.py -p /dev/cu.usbserial-10 -app minihub_lcd flash monitor
  ./tools/build_firmware.py build flash monitor
  ./tools/build_firmware.py build merge
  ./tools/build_firmware.py vibe
  ./tools/build_firmware.py module my_native
  ./tools/build_firmware.py modules
  ./tools/build_firmware.py pack my_native
""",
        file=sys.stderr if code else sys.stdout,
    )
    raise SystemExit(code)


def parse_argv(argv: list[str]) -> tuple[str, list[str], list[str], bool]:
    """Return (app, stacks, idf_args, release)."""
    if argv and argv[0] in {"-h", "--help", "help"}:
        usage(0)

    stacks: list[str] = []
    idf_args: list[str] = []
    release = False
    # `seen_sep` marks the pass-through region: the first unrecognised argument
    # (typically idf.py's -p PORT) starts it. `-app` must still be parsed there,
    # otherwise `-p PORT -app NAME ...` silently falls back to the default app
    # and passes "-app NAME" straight to idf.py. Only a literal `--` stops
    # parsing entirely.
    seen_sep = False
    explicit_sep = False
    cli_app: str | None = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if explicit_sep:
            idf_args.append(a)
            i += 1
            continue
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
        if a in {"-app", "--app"}:
            if i + 1 >= len(argv):
                print("-app requires an application/ folder name (e.g. minihub)", file=sys.stderr)
                usage(2)
            cli_app = argv[i + 1]
            i += 2
            continue
        if a.startswith("--app="):
            cli_app = a.split("=", 1)[1]
            i += 1
            continue
        if a.startswith("-app="):
            cli_app = a.split("=", 1)[1]
            i += 1
            continue
        if a in {"--release", "-release"}:
            release = True
            i += 1
            continue
        if a == "--":
            explicit_sep = True
            seen_sep = True
            i += 1
            continue
        if not seen_sep and a in {"module", "modules", "pack", "lint", "clang-tidy"}:
            app = resolve_app(cli_app)
            rest = argv[i + 1 :]
            # `modules` compiles every native plugin .so (no ids). `pack` only wraps.
            if a in {"lint", "clang-tidy"}:
                return app, [], ["__lint__", *rest], release
            if a == "modules":
                if rest:
                    print(
                        "ERROR: modules takes no ids; it compiles every native plugin. "
                        "Use: ./tools/build_firmware.py module <id>",
                        file=sys.stderr,
                    )
                    usage(2)
                return app, [], ["__module__"], release
            verb = "__pack__" if a == "pack" else "__module__"
            return app, [], [verb, *rest], release
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
        stacks = list(DEFAULT_STACKS)
    else:
        app = resolve_app(cli_app)
    return app, stacks, idf_args, release


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


def run_merge_bin(out_name: str | None = None) -> int:
    """Merge partition images listed in build/flasher_args.json into one flashable bin.

    Output is ``application/<app>/build/<app>_merged.bin`` so every product
    (minihub, minihub_lcd, …) gets a name that ``gen_flash_binaries.py`` can find.
    """
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

    out_path = BUILD_DIR / (out_name or f"{APP_NAME}_merged.bin")
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
    """After a successful firmware build, pack JS modules already in the tree.

    Native plugins stay out of this step. Compile one with
    ``./tools/build_firmware.py module <id>``.
    """
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
        return pack_modules(["all"], skip_native=True)
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
            "thread and zigbee both linked (IEEE 802.15.4). "
            "adapters.json enabled flags decide which runs; "
            "if both are enabled, the one that starts later is skipped.",
            flush=True,
        )


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

    Unselected ble_mesh / zigbee / matter stay untouched (menuconfig may keep
    them). Thread is special:
    - Leaving CONFIG_OPENTHREAD_ENABLED=y after THREAD_ADAPTER is cleared still
      links libopenthread_br and clashes with Matter CHIP route hooks — when
      thread is not in the stack list, force both off.
    - When thread IS linked:
      * CHIP CONFIG_ENABLE_ROUTE_HOOK must stay off: OT BR already defines
        lwip_hook_ip6_route / lwip_hook_nd6_get_gw (RainMaker OTBR pattern).
      * CHIP CONFIG_ENABLE_WIFI_STATION must stay off: Host uplink is Ethernet;
        CHIP must not call esp_wifi_connect() beside MiniHub.
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
    if "thread" not in stacks:
        for sym in ("CONFIG_MINIHUB_THREAD_ADAPTER", "CONFIG_OPENTHREAD_ENABLED"):
            off = f"# {sym} is not set"
            new_text, n = re.subn(
                rf"(?m)^(?:#\s*)?{re.escape(sym)}(?:=y|=n|\s+is not set)\s*$",
                off,
                text,
            )
            if n:
                text = new_text
                changed = True
            elif f"{sym}=y" in text:
                text = text.rstrip() + "\n" + off + "\n"
                changed = True
    else:
        # Thread BR owns lwip hooks and Host uplink is ETH — keep CHIP's
        # Wi-Fi station / route-hook copies out of the link and runtime.
        for sym in ("CONFIG_ENABLE_ROUTE_HOOK", "CONFIG_ENABLE_WIFI_STATION"):
            off = f"# {sym} is not set"
            new_text, n = re.subn(
                rf"(?m)^(?:#\s*)?{re.escape(sym)}(?:=y|=n|\s+is not set)\s*$",
                off,
                text,
            )
            if n:
                text = new_text
                changed = True
            elif f"{sym}=y" in text:
                text = text.rstrip() + "\n" + off + "\n"
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
    """Leave RainMaker to Component Manager.

    espressif/esp_rainmaker is declared in
    components/minihub_rainmaker/idf_component.yml and downloaded into
    managed_components when CONFIG_MINIHUB_RAINMAKER=y. The old path
    application/minihub/components/esp-rainmaker is not in .gitmodules;
    `git submodule update` fails with a pathspec error, and wiring that tree
    into the build would duplicate the registry package.
    """
    dest = rainmaker_managed_dir()
    if (dest / "components").is_dir():
        print(f"esp-rainmaker: local tree present at {dest}")
        return
    print(
        "esp-rainmaker: Component Manager fetches espressif/esp_rainmaker "
        "(components/minihub_rainmaker/idf_component.yml)"
    )


def rainmaker_defaults_files() -> list[Path]:
    """RainMaker fragments, shared first and the product file after it."""
    try:
        rels = feature_layers(ROOT, APP_NAME, "rainmaker")
    except FileNotFoundError:
        return []
    return [ROOT / rel for rel in rels if (ROOT / rel).is_file()]


def rainmaker_build_enabled() -> bool:
    """Whether to link full esp-rainmaker (submodule when enabled).

    Force with MINIHUB_BUILD_RAINMAKER=1|0. Otherwise follow rainmaker.defaults.
    A product fragment overrides the shared fragment per CONFIG_* symbol.
    """
    env = os.environ.get("MINIHUB_BUILD_RAINMAKER")
    if env is not None:
        return env.strip().lower() in {"1", "true", "yes"}
    enabled = False
    for path in rainmaker_defaults_files():
        for sym, line in _rainmaker_default_assignments(path.read_text(encoding="utf-8")):
            if sym == "CONFIG_MINIHUB_RAINMAKER":
                enabled = line == "CONFIG_MINIHUB_RAINMAKER=y"
    return enabled


def _rainmaker_default_assignments(text: str) -> list[tuple[str, str]]:
    """Return (symbol, sdkconfig line) from a defaults fragment."""
    out: list[tuple[str, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("#") and line.endswith("is not set"):
            parts = line[1:].strip().split()
            if parts and parts[0].startswith("CONFIG_"):
                out.append((parts[0], f"# {parts[0]} is not set"))
            continue
        if not line or line.startswith("#") or "=" not in line:
            continue
        sym, _val = line.split("=", 1)
        if sym.startswith("CONFIG_"):
            out.append((sym, line))
    return out


def _apply_defaults_fragment_to_sdkconfig(sources: list[Path]) -> bool:
    """Rewrite product sdkconfig lines from defaults fragments. Return True if changed."""
    dest = APP_DIR / "sdkconfig"
    if not sources or not dest.is_file():
        return False
    text = dest.read_text(encoding="utf-8", errors="replace")
    changed = False
    for src in sources:
        for sym, replacement in _rainmaker_default_assignments(src.read_text(encoding="utf-8")):
            pattern = rf"(?m)^(?:#\s*)?{re.escape(sym)}(?:=.*|\s+is not set)\s*$"
            new_text, n = re.subn(pattern, replacement, text)
            if n == 0 and replacement not in text:
                text = text.rstrip() + "\n" + replacement + "\n"
                changed = True
            elif n and new_text != text:
                text = new_text
                changed = True
    if changed:
        dest.write_text(text, encoding="utf-8")
        print(
            "sdkconfig: applied "
            + ", ".join(src.relative_to(ROOT).as_posix() for src in sources),
            flush=True,
        )
    return changed


def sync_rainmaker_defaults_in_sdkconfig() -> None:
    """Copy rainmaker.defaults into the product sdkconfig.

    SDKCONFIG_DEFAULTS does not replace a choice already saved in sdkconfig.
    RainMaker's Kconfig default is assisted claim, so a product sdkconfig
    created before this fragment was on the recipe keeps
    CONFIG_ESP_RMAKER_SELF_CLAIM unset. Rewrite the saved lines from the
    fragment whenever RainMaker is part of the build.
    """
    if not rainmaker_build_enabled():
        return
    _apply_defaults_fragment_to_sdkconfig(rainmaker_defaults_files())


def release_defaults_files() -> list[Path]:
    """Shared then product release.defaults layers that exist on disk."""
    out: list[Path] = []
    for rel in feature_layers(ROOT, APP_NAME, "release"):
        path = ROOT / rel
        if path.is_file():
            out.append(path)
    return out


def release_build_enabled(cli_release: bool = False) -> bool:
    """True when --release was passed or app.yml lists the release feature."""
    if cli_release:
        return True
    features, _board = app_feature_list(ROOT, APP_NAME)
    return "release" in features


def sync_release_defaults_in_sdkconfig(*, cli_release: bool = False) -> None:
    """Copy release.defaults into the product sdkconfig when release is on.

    Same reason as RainMaker: SDKCONFIG_DEFAULTS cannot flip a choice already
    saved as LOG_DEFAULT_LEVEL_INFO. Rewrite from the fragment so Warning
    actually sticks.
    """
    if not release_build_enabled(cli_release):
        return
    _apply_defaults_fragment_to_sdkconfig(release_defaults_files())


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


def ensure_zigbee_stack(stacks: list[str]) -> list[str]:
    """If ZIGBEE_SDK=y in sdkconfig, pull in the zigbee stack automatically.

    Parity with ensure_thread_stack: a stack enabled in sdkconfig is linked
    even when the CLI only named the other 802.15.4 stack. Runtime
    adapters.json decides which one starts.
    """
    if "zigbee" in stacks:
        return stacks
    if not sdkconfig_has("CONFIG_MINIHUB_ZIGBEE_SDK"):
        return stacks
    out = list(stacks)
    out.append("zigbee")
    print(
        "CONFIG_MINIHUB_ZIGBEE_SDK=y → adding stack 'zigbee' "
        "(clear the Kconfig option to build without Zigbee)",
        flush=True,
    )
    return out


def sdkconfig_defaults(stacks: list[str], *, release: bool = False) -> str:
    # Do not list board_manager.defaults here. esp_board_manager 0.5+ injects it.
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
    if release_build_enabled(release) and "release" not in features:
        features.append("release")
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
    """Product-local gen_bmgr_codes (application/<app>/components/, gitignored)."""
    dest = APP_DIR / "components" / "gen_bmgr_codes"
    dest.mkdir(parents=True, exist_ok=True)
    return dest


def ensure_gen_bmgr_placeholder() -> None:
    """Create a minimal gen_bmgr_codes component so set-target/cmake can resolve REQUIRES.

    Fresh CI: application/<app>/components/gen_bmgr_codes/ is gitignored.
    Real codes are generated after Component Manager fetches esp_board_manager
    (see shell_bmgr_codegen).
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
    # A product-local board must actually be the one bmgr used: the generated
    # board_manager.defaults carries "# Source: <board dir>/sdkconfig.defaults.board".
    # Probing for CONFIG_SPIRAM here was always true (board defaults never carry it),
    # which re-ran codegen on every build once boards/ existed.
    defaults_text = defaults.read_text(encoding="utf-8") if defaults.is_file() else ""
    customer_board = boards_dir / bmgr_name
    local_boards_stale = customer_board.is_dir() and (
        str(customer_board.resolve()) not in defaults_text
    )
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


def run_build(
    stacks: list[str],
    idf_args: list[str],
    *,
    pack_after: bool = True,
    release: bool = False,
) -> int:
    local_verbs, idf_args = split_local_verbs(idf_args)
    # --lint: run clang-tidy over first-party sources after a successful build.
    do_lint = "--lint" in idf_args or os.environ.get("MINIHUB_LINT") in {"1", "true", "yes"}
    if "--lint" in idf_args:
        idf_args = [a for a in idf_args if a != "--lint"]

    # merge-only: no idf.py invocation
    if not idf_args:
        if "merge" in local_verbs:
            return run_merge_bin()
        print("ERROR: no idf.py action (e.g. build / flash) and no local verb", file=sys.stderr)
        usage(2)

    stacks = drop_sdkconfig_disabled_stacks(stacks)
    stacks = ensure_matter_stack(stacks)
    stacks = ensure_thread_stack(stacks)
    stacks = ensure_zigbee_stack(stacks)
    validate_stacks(stacks)
    write_stacks_gen(stacks)
    sync_stack_symbols_in_sdkconfig(stacks)
    sync_radio_memory_in_sdkconfig(stacks)
    sync_rainmaker_defaults_in_sdkconfig()
    sync_release_defaults_in_sdkconfig(cli_release=release)
    use_release = release_build_enabled(release)
    defaults = sdkconfig_defaults(stacks, release=release)
    idf_path = Path(os.environ.get("IDF_PATH", str(ROOT / "esp-idf")))
    espbaud = os.environ.get("ESPBAUD", "2000000")
    use_matter = "matter" in stacks
    use_rainmaker = rainmaker_build_enabled()

    print(f"product={APP_NAME} ({APP_DIR})")
    print(f"stacks={','.join(stacks)}")
    print(f"MINIHUB_APP={APP_NAME}")
    print(f"MINIHUB_BUILD_MATTER={1 if use_matter else 0}")
    print(f"MINIHUB_BUILD_RAINMAKER={1 if use_rainmaker else 0}")
    print(f"MINIHUB_RELEASE={1 if use_release else 0}")
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
    if use_rainmaker:
        ensure_esp_rainmaker()
    if sdkconfig_has("CONFIG_MINIHUB_MCP_SERVER"):
        ensure_git_submodule("components/esp-claw")
    # Early patch pass (no-op skip when managed_components not fetched yet).
    # Fresh CI: CM download happens inside set-target / reconfigure / build below —
    # re-apply again just before the final idf.py so Soft SHA etc. land on disk.
    subprocess.run(
        [str(ROOT / "tools/apply_component_patches.sh")],
        cwd=str(ROOT),
        env={**os.environ, "MINIHUB_APP_DIR": str(APP_DIR)},
        check=True,
    )
    # Placeholder first: app has a symlink to gitignored components/gen_bmgr_codes/.
    # Creating the target lets board-manager os.makedirs(exist_ok=True) succeed
    # (dangling symlink → Errno 17 File exists).
    ensure_gen_bmgr_placeholder()
    ensure_board_manager()

    idf_cmd = "idf.py --preview " + " ".join(shlex.quote(a) for a in idf_args)
    patch_cmd = (
        f'export MINIHUB_APP_DIR={shlex.quote(str(APP_DIR))} && '
        f"{shlex.quote(str(ROOT / 'tools/apply_component_patches.sh'))}"
    )
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
    # Managed RainMaker (and peers) exist only after the cmake steps above.
    shell_parts.append(
        'echo "Re-applying managed component patches after Component Manager fetch"'
    )
    shell_parts.append(patch_cmd)
    shell_parts.append(idf_cmd)

    # vibe_mode.bin is built by the Host CMake target ``vibe_mode``
    # (components/minihub_vibe/project_include.cmake), same pattern as
    # bootloader — no pre-mkdir of Host build/ and no post-copy register step.
    if needs_esp32s31_set_target():
        scrub_non_cmake_host_build()

    script = "\n".join(shell_parts) + "\n"
    rc = subprocess.run(["bash", "-c", script], cwd=str(APP_DIR)).returncode
    if pack_after:
        rc = maybe_pack_modules_after_build(idf_args, rc)
    if rc == 0 and "merge" in local_verbs:
        print()
        rc = run_merge_bin()
    if do_lint and rc == 0:
        print()
        rc = run_lint([])
    return rc


def scrub_non_cmake_host_build() -> None:
    """Remove a Host build/ that is not an IDF CMake tree.

    ``idf.py set-target`` depends on ``fullclean``, which exits 1 when build/
    exists but has no CMakeCache.txt (refuses to delete unknown contents).
    """
    build = APP_DIR / "build"
    if not build.is_dir() or (build / "CMakeCache.txt").is_file():
        return
    print(f"Removing non-CMake {build} before idf.py set-target")
    shutil.rmtree(build)


def _store_verb_args(raw: list[str]) -> tuple[Path | None, list[str]]:
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
    return deploy, ids


def run_pack(raw: list[str]) -> int:
    deploy, ids = _store_verb_args(raw)
    packing_all = not ids or ids == ["all"]
    try:
        return pack_modules(
            ids if ids else ["all"],
            deploy=deploy,
            skip_native=packing_all,
        )
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        return exc.returncode or 1


def run_module_compile(raw: list[str]) -> int:
    """Rebuild native module shared objects (idf target module_so_<id>)."""
    deploy, ids = _store_verb_args(raw)
    if deploy is not None:
        print("ERROR: --deploy applies to pack, not module", file=sys.stderr)
        return 2
    if not ids or ids == ["all"]:
        items = [(mid, kind) for mid, kind in resolve_module_ids(["all"]) if kind == "binary"]
        if not items:
            print("ERROR: no native modules to compile", file=sys.stderr)
            return 2
    else:
        items = resolve_module_ids(ids)
    targets: list[str] = []
    for mid, kind in items:
        if kind != "binary":
            print(
                f"ERROR: {mid} has no native .so. Pack it with: "
                f"./tools/build_firmware.py pack {mid}",
                file=sys.stderr,
            )
            return 2
        targets.append(f"module_so_{mid}")
    # `idf.py build` is the ninja `all` target (app + bootloader). These names are
    # ninja targets on their own; passing them without `build` compiles only the .so.
    return run_build(list(DEFAULT_STACKS), targets, pack_after=False)


# First-party scope mirrors the .clang-tidy HeaderFilterRegex:
# only components/minihub_* and application/<app>/main are MiniHub-owned code.
_FIRST_PARTY_RE = re.compile(r"components/minihub_.*|application/[^/]+/main/")
# Detect the GCC toolchain path so we can point clang at its newlib sysroot.
_GCC_RE = re.compile(r"(.*)/bin/[^/]*-gcc$")

# GCC / ESP-only flags that stock clang (from Homebrew/apt LLVM) rejects outright.
# clang-tidy parses with the clang frontend, which does not know these.
_SANITIZE_DROP_EXACT = {
    "-fno-tree-switch-conversion",
    "-fstrict-volatile-bitfields",
    "-fzero-init-padding-bits=all",
    "-fno-malloc-dce",
    "-mno-cm-popret",
    "-freorder-blocks",
    "-menable-experimental-extensions",
    "-fconserve-stack",
    "-fno-delete-null-pointer-checks",
    "-fno-allow-store-data-races",
    "-fno-var-tracking-assignments",
    "-femit-struct-debug-baseonly",
    "-fno-ipa-sra",
    "-fno-ipa-cp-clone",
    "-fno-partial-inlining",
    "-fno-optimize-sibling-calls",
    "-fno-tree-vrp",
    "-fno-tree-dse",
    "-fallocation-dce",
    "-fno-tree-dominator-opts",
    "-fno-tree-cselim",
    "-fno-tree-phiprop",
    "-fno-tree-ccp",
    "-fno-tree-copy-prop",
    "-fno-tree-dce",
    "-fno-tree-fre",
    "-fno-tree-sink",
    "-fno-tree-loop-if-convert",
    "-fno-tree-loop-distribution",
    "-fno-tree-loop-vectorize",
    "-fno-tree-slp-vectorize",
    "-fno-tree-switch-conversion",
    "-fno-tree-tail-merge",
    "-fno-tree-pre",
    "-fno-tree-forwprop",
    "-fno-tree-phiopt",
    "-fno-tree-pta",
    "-fno-tree-ealias",
    "-fno-tree-store-ccp",
    "-fno-tree-store-copy-prop",
    "-fgcse",
    "-fgcse-sm",
    "-fgcse-las",
    "-fno-tree-loop-im",
    "-fno-tree-loop-unswitch",
    "-fno-tree-loop-unroll",
    "-fno-tree-loop-ivcanon",
    "-fno-tree-loop-optimize",
    "-fno-tree-ch",
    "-fno-tree-ssa",
    "-fno-tree-coalesce-vars",
    "-fno-tree-copyrename",
    "-fno-tree-sra",
    "-fno-tree-ter",
    "-fno-tree-lrs",
    "-fno-tree-sink",
    "-fno-tree-dse",
    "-fno-tree-dce",
}

_WARN_VALUE_RE = re.compile(r"^-W[^=]+=.*$")


def _sanitize_command(cmd: str, clang: str = "clang") -> str:
    """Rewrite a GCC compile command so stock clang can parse it.

    Strips GCC/ESP-only flags, neutralises the ESP RISC-V target/extensions, and
    switches the driver to a generic ``riscv32`` so clang-tidy can build an AST.
    The goal is parseability, not faithful codegen — clang-tidy only needs the AST.

    The original compiler token (position 0) is replaced by *clang* (the sibling of
    clang-tidy). clang-tidy discards position 0 anyway, but keeping a real clang
    path there prevents it from mis-parsing the command and falling back to an
    auto-detected compile_commands.json. ``-target riscv32`` is then a normal flag
    after the compiler.

    GCC ``@file`` response files (used by IDF's toolchain/cflags) are expanded and
    sanitised too — they commonly hide the ESP ``-march``/``-mcpu`` flags.
    """
    try:
        toks = shlex.split(cmd)
    except ValueError:
        toks = cmd.split()
    if not toks:
        return cmd
    # Expand @file response files into the flat token list.
    expanded: list[str] = []
    for t in toks:
        if t.startswith("@"):
            rsp = t[1:]
            try:
                text = Path(rsp).read_text(encoding="utf-8", errors="replace")
                expanded.extend(shlex.split(text))
            except OSError:
                expanded.append(t)  # leave as-is if unreadable
        else:
            expanded.append(t)
    rest = expanded[1:]
    out: list[str] = [clang, "-target", "riscv32"]
    i = 0
    n = len(rest)
    while i < n:
        t = rest[i]
        if t in _SANITIZE_DROP_EXACT:
            i += 1
            continue
        # Drop all -Wno-* (unknown to stock clang => hard error) and -W<name>=<val>.
        if t.startswith("-Wno-") or _WARN_VALUE_RE.match(t):
            i += 1
            continue
        # Drop -Werror so compiler warnings don't fail clang-tidy.
        if t == "-Werror" or t.startswith("-Werror="):
            i += 1
            continue
        # Rewrite the ESP RISC-V -march (xesploop/xespv extensions are unknown).
        if t.startswith("-march="):
            out.append("-march=rv32imac_zicsr_zifencei")
            i += 1
            continue
        if t == "-march":
            if i + 1 < n:
                out.append("-march=rv32imac_zicsr_zifencei")
                i += 2
                continue
        # Neutralise the unknown -mcpu / -mtune (esp32s31).
        if t.startswith("-mcpu=") or t.startswith("-mtune="):
            i += 1
            continue
        if t == "-mcpu" or t == "-mtune":
            i += 2
            continue
        # Drop GCC -specs= files (picolibc.specs etc.) — unused by clang.
        if t.startswith("-specs="):
            i += 1
            continue
        out.append(t)
        i += 1
    # Point clang at the GCC toolchain's newlib include dirs so system headers
    # (e.g. <sys/features.h>) resolve — clang does not auto-add gcc's sysroot.
    for t in expanded:
        m = _GCC_RE.search(t)
        if m:
            root = m.group(1)
            out.extend(
                [
                    "-isystem",
                    f"{root}/riscv32-esp-elf/include",
                    "-isystem",
                    f"{root}/riscv32-esp-elf/sys-include",
                ]
            )
            break
    return shlex.join(out)


def _write_sanitized_compile_commands(src: Path, dst_dir: Path, clang: str = "clang") -> Path:
    """Copy compile_commands.json, sanitising each command for stock clang."""
    data = json.loads(src.read_text(encoding="utf-8"))
    for entry in data:
        if "command" in entry and entry["command"]:
            entry["command"] = _sanitize_command(entry["command"], clang)
    dst = dst_dir / "compile_commands.json"
    dst.write_text(json.dumps(data, indent=1), encoding="utf-8")
    return dst


def _find_esp_clang_tidy() -> str | None:
    """Return an ESP-patched clang-tidy (under ~/.espressif/tools/esp-clang) if present."""
    base = Path(os.path.expanduser("~")) / ".espressif" / "tools" / "esp-clang"
    if base.is_dir():
        hits = sorted(base.glob("*/*/bin/clang-tidy"))
        if hits:
            return str(hits[0])
    return None


def _project_sources_from_compile_commands(cc: Path) -> list[str]:
    """First-party source files (components/minihub_* and application/<app>/main)."""
    data = json.loads(cc.read_text(encoding="utf-8"))
    seen: set[str] = set()
    out: list[str] = []
    for entry in data:
        f = entry.get("file")
        if not f:
            continue
        ap = os.path.abspath(f)
        if not _FIRST_PARTY_RE.search(ap):
            continue
        if ap in seen:
            continue
        seen.add(ap)
        out.append(ap)
    return sorted(out)


def run_lint(raw: list[str]) -> int:
    """Run clang-tidy over project sources using the IDF build's compile_commands.json.

    Usage::

        ./tools/build_firmware.py lint                 # all first-party sources
        ./tools/build_firmware.py lint path/to/file.c  # one or more files
        ./tools/build_firmware.py lint -j 8           # parallelism

    Requires a successful build first (writes
    ``application/<app>/build/compile_commands.json``). The repo-root ``.clang-tidy``
    is auto-discovered; clang-tidy's exit code propagates so CI can gate on it.
    """
    cc = BUILD_DIR / "compile_commands.json"
    if not cc.is_file():
        print(
            "ERROR: compile_commands.json not found at "
            f"{cc}.\nRun a build first: ./tools/build_firmware.py build",
            file=sys.stderr,
        )
        return 1

    # Parse -j/--jobs and explicit file args.
    jobs = int(os.environ.get("MINIHUB_LINT_JOBS", "4"))
    explicit: list[str] = []
    i = 0
    while i < len(raw):
        a = raw[i]
        if a in ("-j", "--jobs") and i + 1 < len(raw):
            jobs = int(raw[i + 1])
            i += 2
            continue
        if a.startswith("--jobs="):
            jobs = int(a.split("=", 1)[1])
            i += 1
            continue
        explicit.append(a)
        i += 1

    if explicit:
        files = sorted({str(Path(a).resolve()) for a in explicit})
    else:
        files = _project_sources_from_compile_commands(cc)
        if not files:
            print("No first-party sources found in compile_commands.json.", file=sys.stderr)
            return 1

    # Prefer the ESP-patched clang-tidy (knows esp32s31). Fall back to stock
    # clang-tidy and sanitise compile_commands so stock clang can parse it.
    clang_tidy = _find_esp_clang_tidy()
    cc_dir = BUILD_DIR
    tmpdir = None
    if clang_tidy is None:
        clang_tidy = shutil.which("clang-tidy")
        if clang_tidy is None:
            print(
                "ERROR: clang-tidy not found on PATH. Install it:\n"
                "  macOS:   brew install llvm            (stock; parses via sanitised flags)\n"
                "  Linux:   sudo apt install clang-tidy\n"
                "  preferred: idf_tools.py install esp-clang   (native esp32s31 support)\n"
                "Then re-run with clang-tidy on PATH, e.g.\n"
                '  export PATH="$(brew --prefix llvm)/bin:$PATH"',
                file=sys.stderr,
            )
            return 1
        tmpdir = tempfile.mkdtemp(prefix="minihub-lint-")
        clang_bin = str(Path(clang_tidy).with_name("clang"))
        if not os.path.isfile(clang_bin):
            clang_bin = "clang"
        _write_sanitized_compile_commands(cc, Path(tmpdir), clang_bin)
        cc_dir = Path(tmpdir)

    print("=== clang-tidy ===")
    print(f"clang-tidy : {clang_tidy}")
    print(f"build dir  : {BUILD_DIR}")
    print(f"sources    : {len(files)} file(s)")
    print(f"jobs       : {jobs}")
    print("config     : auto-discovered .clang-tidy at repo root")
    if tmpdir:
        print("note       : stock clang-tidy — compile_commands sanitised for parseability")
    print(flush=True)

    failed: list[str] = []
    lock = threading.Lock()

    def _run(fpath: str) -> int:
        proc = subprocess.run(
            [clang_tidy, "-p", str(cc_dir), fpath],
            cwd=str(ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        out = proc.stdout or ""
        with lock:
            if out.strip():
                print(out.rstrip(), flush=True)
            if proc.returncode != 0:
                failed.append(fpath)
        return proc.returncode

    try:
        with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
            list(ex.map(_run, files))
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)

    print(flush=True)
    if failed:
        print(
            f"clang-tidy reported issues in {len(failed)} file(s).",
            file=sys.stderr,
        )
        return 1
    print(f"clang-tidy: clean ({len(files)} file(s), 0 issues).")
    return 0


def main() -> None:
    # Vibe Mode factory image (components/minihub_vibe/subproject). Accept both
    #   ./tools/build_firmware.py vibe ...
    #   ./tools/build_firmware.py -app minihub_lcd vibe ...
    # before parse_argv() would treat "vibe" as an idf.py argument.
    raw = sys.argv[1:]
    if "vibe" in raw:
        vibe_i = raw.index("vibe")
        # Keep -app / --app ahead of vibe for build_vibe_mode.py; drop the
        # "vibe" token itself (the script has no such flag).
        vibe_argv = raw[:vibe_i] + raw[vibe_i + 1 :]
        raise SystemExit(subprocess.run(
            [sys.executable, str(ROOT / "tools" / "build_vibe_mode.py"), *vibe_argv],
            cwd=str(ROOT),
        ).returncode)
    app, stacks, idf_args, release = parse_argv(raw)
    set_app(app)
    os.environ["MINIHUB_APP"] = APP_NAME
    if idf_args and idf_args[0] == "__pack__":
        raise SystemExit(run_pack(idf_args[1:]))
    if idf_args and idf_args[0] == "__module__":
        raise SystemExit(run_module_compile(idf_args[1:]))
    if idf_args and idf_args[0] == "__lint__":
        raise SystemExit(run_lint(idf_args[1:]))
    raise SystemExit(run_build(stacks, idf_args, release=release))


if __name__ == "__main__":
    main()

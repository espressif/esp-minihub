# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from pathlib import Path

from .ids import check_module_id
from .paths import STORE_MODULES, TEMPLATES

TIERS = ("l1", "l2", "l3")


def _replace(text: str, module_id: str, name: str) -> str:
    return text.replace("PLACEHOLDER_ID", module_id).replace("PLACEHOLDER_NAME", name)


def load_template_files(tier: str, module_id: str, name: str) -> dict[str, str]:
    src = TEMPLATES / tier
    if not src.is_dir():
        raise FileNotFoundError(
            f"missing templates at {src} — run this MCP from an esp-minihub checkout"
        )
    files: dict[str, str] = {}
    for path in sorted(src.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(src).as_posix()
        files[rel] = _replace(path.read_text(encoding="utf-8"), module_id, name)
    if not files:
        raise FileNotFoundError(f"empty template {src}")
    return files


def scaffold_plugin(
    *,
    tier: str,
    module_id: str,
    name: str = "",
    out_dir: str | None = None,
    in_tree: bool = False,
    force: bool = False,
) -> dict:
    tier = tier.strip().lower()
    if tier not in TIERS:
        raise ValueError(f"tier must be one of {TIERS}")
    module_id = module_id.strip()
    err = check_module_id(module_id)
    if err:
        raise ValueError(err)
    display = (name or "").strip() or module_id
    files = load_template_files(tier, module_id, display)

    dest: Path | None = None
    if in_tree:
        dest = STORE_MODULES / module_id
    elif out_dir:
        dest = Path(out_dir).expanduser().resolve()

    written: list[str] = []
    if dest is not None:
        if dest.exists() and any(dest.iterdir()) and not force:
            raise FileExistsError(f"{dest} already exists (pass force=true to replace)")
        dest.mkdir(parents=True, exist_ok=True)
        for rel, text in files.items():
            path = dest / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            written.append(str(path))

    next_steps = [
        "Call validate_plugin on the module directory.",
        "For L1/L2: call pack_plugin to write <id>.mhub.",
    ]
    if tier == "l3":
        next_steps.append(
            "L3: build Host (IDF 6.2 / esp32s31) so the MHMD .so exists, then pack in the firmware tree."
        )

    return {
        "ok": True,
        "tier": tier,
        "id": module_id,
        "name": display,
        "out_dir": str(dest) if dest else None,
        "files": files,
        "written": written,
        "next": next_steps,
    }

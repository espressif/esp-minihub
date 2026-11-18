# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage the component map: every ``components/minihub_*`` folder, by job.

The lane grouping follows ``docs/en/source_tree.rst`` ("``components/``, by job"),
and the build re-reads the directory: a component that appears here but not on
disk, or exists on disk but is not placed in a lane, stops the docs build. That is
the point — the map cannot quietly fall behind a refactor that adds or renames a
component.

The arrow down the left edge is the dependency direction: a lane may depend on the
lanes below it and on nothing above it.
"""

from __future__ import annotations

from pathlib import Path

from sphinx.errors import ExtensionError

from minihub_figure_kit import grid_figure, write_text

ROOT = Path(__file__).resolve().parents[2]
_STAGED = ROOT / "docs" / "_static" / "source_tree"

TITLE = "MiniHub 组件按职责 · components/ by job"
LEAD = "components/ 下的目录，按职责分成五行。"

LANES = [
    ("Core and services", "核心与服务", [
        "minihub_core", "minihub_config", "minihub_hub", "minihub_auth",
        "minihub_adapter", "minihub_provision", "minihub_net", "minihub_ota",
        "minihub_vibe", "minihub_update", "minihub_mqtt_broker",
    ]),
    ("Device model and UI", "设备模型与界面", [
        "minihub_device_widget", "minihub_rules", "minihub_scenes",
        "minihub_product_params", "minihub_web", "minihub_mcp_server",
    ]),
    ("Store and modules", "商店与模块", [
        "minihub_store", "minihub_module", "minihub_pack", "minihub_js",
    ]),
    ("Radios", "射频", [
        "minihub_zigbee", "minihub_ble_mesh", "minihub_ble_gatt",
        "minihub_matter", "minihub_thread", "minihub_rainmaker",
        "minihub_protocol_adapters",
    ]),
    ("Optional hardware", "可选硬件", [
        "minihub_lcd", "minihub_rtsp_cam", "minihub_usb_msc",
    ]),
]

LEGEND = ()


def on_disk() -> set[str]:
    return {p.name for p in (ROOT / "components").glob("minihub_*") if p.is_dir()}


def check() -> list[str]:
    placed = [name for _e, _z, cards in LANES for name in cards]
    problems = []
    for name in placed:
        if not (ROOT / "components" / name).is_dir():
            problems.append(f"{name} is in a lane but has no directory")
    dupes = {n for n in placed if placed.count(n) > 1}
    if dupes:
        problems.append(f"placed twice: {', '.join(sorted(dupes))}")
    missing = sorted(on_disk() - set(placed))
    if missing:
        problems.append("components not placed in any lane: " + ", ".join(missing))
    extra = sorted(set(placed) - on_disk())
    if extra:
        problems.append("lanes cite components that no longer exist: " + ", ".join(extra))
    return problems


def stage_source_grid_diagram() -> Path:
    """Write the component map, refusing to stage a stale one."""
    problems = check()
    if problems:
        raise ExtensionError("stage_source_grid is out of date:\n  "
                             + "\n  ".join(problems))
    _STAGED.mkdir(parents=True, exist_ok=True)
    title, subtitle = TITLE.split(" · ")
    svg = grid_figure(title, subtitle, LEAD, LANES, LEGEND)
    write_text(_STAGED / "component_map.svg", svg)
    return _STAGED

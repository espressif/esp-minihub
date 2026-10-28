#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Gate the generated flow figures: no clipped text, no silent truncation.

Run directly (it imports the docs extensions, so it needs the docs venv), after a
docs build has staged ``docs/_static``:

    ./tools/build_docs.sh all
    .venv-docs/bin/python tools/test_doc_figures.py

A card that cannot fit its note used to drop the overflow off the right edge, and
a wrap limit cut the rest of the sentence away with no warning — both read as a
figure that quietly contradicts the prose next to it. The SVG root also has to
carry width/height: without an intrinsic size an <img> falls back to a default
box, the figure's aspect ratio is lost, and the drawing ends up letterboxed.
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docs" / "_extensions"))

from minihub_figure_kit import (  # noqa: E402
    CARD_TOP,
    FIG_W,
    GAP,
    MARGIN,
    text_width,
)
from stage_net_boot import BOOT_STAGES, SVG_NAME as NET_SVG_NAME  # noqa: E402
from stage_net_boot import stage_net_boot_diagram  # noqa: E402
from stage_ota_flow import SVG_NAME as OTA_SVG_NAME  # noqa: E402
from stage_ota_flow import UPDATE_STAGES, stage_ota_update_diagram  # noqa: E402
from stage_source_grid import stage_source_grid_diagram  # noqa: E402

NS = "{http://www.w3.org/2000/svg}"
# The published guide carries no tier wording, and neither may a figure.
TIER_TOKENS = ("L3", "minihub-plugin-l3", "develop_l3", "MHMD", "dlopen", "dlclose")


def check_svg(path: Path, stages) -> list[str]:
    problems = []
    root = ET.parse(path).getroot()
    if not root.get("width") or not root.get("height"):
        problems.append(f"{path.name}: no intrinsic width/height on the <svg> root")
    vb = root.get("viewBox", "").split()
    if len(vb) == 4 and root.get("width") and root.get("height"):
        if round(float(vb[2]) / float(vb[3]), 3) != round(_ratio(root), 3):
            problems.append(f"{path.name}: viewBox aspect differs from width/height")
    n = len(stages)
    card_w = (FIG_W - 2 * MARGIN - (n - 1) * GAP) / n
    card_b = CARD_TOP + 230
    per_card: dict[int, list[str]] = {}
    for node in root.iter(f"{NS}text"):
        raw = node.text or ""
        for token in TIER_TOKENS:
            if token in raw:
                problems.append(f"{path.name}: tier wording {token!r} in {raw[:40]!r}")
        x = float(node.get("x", "0"))
        y = float(node.get("y", "0"))
        w = text_width(raw, float(node.get("font-size", "13.5")))
        if CARD_TOP - 5 <= y <= card_b:
            idx = int((x - MARGIN) // (card_w + GAP))
            per_card.setdefault(idx, []).append(raw)
            right = MARGIN + idx * (card_w + GAP) + card_w
            if x + w > right:
                problems.append(f"{path.name}: card {idx} text overflows by "
                                f"{round(x + w - right)}px: {raw[:34]!r}")
        elif x + w > FIG_W - MARGIN:
            problems.append(f"{path.name}: page text overflows by "
                            f"{round(x + w - (FIG_W - MARGIN))}px: {raw[:34]!r}")
    # Whatever the wrap decided, no source field may lose a character: a card
    # that quietly drops its tail would contradict the prose beside it.
    for idx, stage in enumerate(stages):
        got = "".join(per_card.get(idx, [])).replace(" ", "")
        want = "".join(stage).replace(" ", "")
        if got != want:
            problems.append(f"{path.name}: card {idx} text incomplete: "
                            f"{want[:30]!r} -> {got[:30]!r}")
    return problems


def _ratio(root) -> float:
    return float(root.get("width")) / float(root.get("height"))


def check_page(path: Path, cards: int) -> list[str]:
    """A page that parses is not a page that renders: an unclosed ``<style>``
    swallows the whole ``<body>``, and the reader gets a blank screen with a
    title. Three of these shipped blank once, so the structure is asserted, not
    assumed."""
    html = path.read_text(encoding="utf-8")
    problems = []
    for url in re.findall(r"(?:src|href)=\"(https?:)[^\"]*\"", html):
        problems.append(f"{path.name}: fetches an external resource ({url})")
    if "本页不请求外部资源" not in html:
        problems.append(f"{path.name}: offline note missing")
    if "</style></head><body>" not in html:
        problems.append(f"{path.name}: <style> is never closed, so the body would not render")
    if html.count('<article class="step"') != cards:
        problems.append(f"{path.name}: {cards} cards expected, "
                        f"found {html.count('<article class=\"step\"')}")
    return problems


def check_all_pages() -> list[str]:
    """Every interactive figure page, not just the two built by the kit: the
    hand-written templates can break the same way and nobody looks at them."""
    problems = []
    for page in sorted((ROOT / "docs" / "_static").glob("*/index.html")):
        html = page.read_text(encoding="utf-8")
        if "</style>" not in html or "<body>" not in html:
            problems.append(f"{page.parent.name}: <style> never closes, the page renders blank")
        if "<article" not in html:
            problems.append(f"{page.parent.name}: no cards in the page")
    return problems


def check_all_staged_svgs() -> list[str]:
    """Every staged figure needs an intrinsic size, not just the new ones."""
    problems = []
    for svg in sorted((ROOT / "docs" / "_static").rglob("*.svg")):
        if "catalog/images" in str(svg):
            continue  # device icons: sized by their own viewBox on purpose
        try:
            root = ET.parse(svg).getroot()
        except ET.ParseError as exc:
            problems.append(f"{svg}: {exc}")
            continue
        if not root.get("width") or not root.get("height"):
            problems.append(f"{svg.relative_to(ROOT)}: no intrinsic width/height")
    return problems


def check_unique_basenames() -> list[str]:
    """Sphinx keys _images/ by basename, so two figures may not share one name."""
    seen: dict[str, list[str]] = {}
    for svg in sorted((ROOT / "docs" / "_static").rglob("*.svg")):
        if "catalog/images" in str(svg):
            continue  # device icons are addressed by path, never copied to _images
        seen.setdefault(svg.name, []).append(str(svg.parent.name))
    return [f"_images/: {name} staged by {dirs}" for name, dirs in seen.items() if len(dirs) > 1]


def check_plain_svg(path: Path) -> list[str]:
    """Intrinsic size, no tier wording, nothing past the right margin."""
    problems = []
    root = ET.parse(path).getroot()
    if not root.get("width") or not root.get("height"):
        problems.append(f"{path.name}: no intrinsic width/height")
    for node in root.iter(f"{NS}text"):
        raw = node.text or ""
        for token in TIER_TOKENS:
            if token in raw:
                problems.append(f"{path.name}: tier wording {token!r} in {raw[:40]!r}")
        x = float(node.get("x", "0"))
        w = text_width(raw, float(node.get("font-size", "13.5")))
        if x + w > FIG_W - MARGIN:
            problems.append(f"{path.name}: text past the right margin by "
                            f"{round(x + w - (FIG_W - MARGIN))}px: {raw[:34]!r}")
    return problems


def main() -> int:
    problems: list[str] = []
    figures = ((stage_ota_update_diagram(), UPDATE_STAGES, OTA_SVG_NAME),
               (stage_net_boot_diagram(), BOOT_STAGES, NET_SVG_NAME))
    for staged, stages, svg_name in figures:
        svg = staged / svg_name
        page = staged / "index.html"
        if not svg.is_file() or not page.is_file():
            problems.append(f"{staged.name}: {svg_name} or index.html missing")
            continue
        problems += check_svg(svg, stages)
        problems += check_page(page, len(stages))
        if not svg.read_text(encoding="utf-8").count("<style>"):
            problems.append(f"{staged.name}: no inline <style>, motion would be lost")
    problems += check_all_staged_svgs()
    problems += check_all_pages()
    problems += check_unique_basenames()
    grid = stage_source_grid_diagram()
    problems += check_plain_svg(grid / "component_map.svg")
    for line in problems:
        print("FAIL:", line)
    if problems:
        return 1
    print(f"test_doc_figures: ok ({len(UPDATE_STAGES) + len(BOOT_STAGES)} cards, "
          f"2 figures, "
          f"{len(list((ROOT / 'docs' / '_static').glob('*/index.html')))} pages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

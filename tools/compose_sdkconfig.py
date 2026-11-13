# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Compose SDKCONFIG_DEFAULTS from shared + per-app sdkconfig fragments.

Product = application/<app>/ folder (MINIHUB_APP), e.g. minihub.
Each product has one recipe: application/<app>/configs/sdkconfig/app.yml
Optional HW recipes: variants/*.yml (copy over app.yml when needed).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None  # type: ignore

SHARED_DIR = "configs/sdkconfig"
APP_SDK_REL = "configs/sdkconfig"  # under application/<app>/
STACKS_GEN_REL = "cmake/generated/sdkconfig_stacks.gen"


def app_sdk_dir(project_root: Path, app_name: str) -> Path:
    return project_root / "application" / app_name / APP_SDK_REL


def shared_sdk_dir(project_root: Path) -> Path:
    return project_root / SHARED_DIR


def load_app_recipe(project_root: Path, app_name: str) -> dict[str, Any]:
    """Load application/<app>/configs/sdkconfig/app.yml."""
    path = app_sdk_dir(project_root, app_name) / "app.yml"
    if not path.is_file():
        raise FileNotFoundError(
            f"Missing sdkconfig recipe for product {app_name!r}: {path}"
        )
    if yaml is None:
        raise RuntimeError("PyYAML required: pip install pyyaml")
    data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    if not isinstance(data, dict):
        raise ValueError(f"Invalid recipe file: {path}")
    return data


def _feature_candidates(app_name: str, name: str) -> tuple[str, str, str]:
    """Return (label, shared path, product path) for one feature name."""
    if name in ("generated/stacks.gen", "stacks.gen", STACKS_GEN_REL):
        return name, STACKS_GEN_REL, ""
    if name in ("common", "memory"):
        return name, f"{SHARED_DIR}/{name}.defaults", (
            f"application/{app_name}/{APP_SDK_REL}/{name}.defaults"
        )
    key = name
    for prefix in ("feature/", "stack/", "shared/"):
        if key.startswith(prefix):
            key = key[len(prefix) :]
            break
    return (
        key,
        f"{SHARED_DIR}/feature/{key}.defaults",
        f"application/{app_name}/{APP_SDK_REL}/feature/{key}.defaults",
    )


def feature_layers(project_root: Path, app_name: str, name: str) -> list[str]:
    """Files for one feature, lowest priority first.

    The file name only groups symbols for people. When both the shared
    fragment and the product fragment exist, both are loaded. The product
    file comes later, so a CONFIG_* line in it replaces the same symbol from
    the shared file. A symbol that only the shared file sets is kept.
    """
    key, shared_rel, app_rel = _feature_candidates(app_name, name)
    if name in ("generated/stacks.gen", "stacks.gen", STACKS_GEN_REL):
        return [STACKS_GEN_REL]
    layers: list[str] = []
    if (project_root / shared_rel).is_file():
        layers.append(shared_rel)
    if app_rel and (project_root / app_rel).is_file():
        layers.append(app_rel)
    if not layers:
        raise FileNotFoundError(
            f"sdkconfig feature {key!r} not found for app={app_name!r} "
            f"(tried {app_rel} and {shared_rel})"
        )
    return layers


def feature_to_path(project_root: Path, app_name: str, name: str) -> str:
    """Highest-priority fragment for one feature (product file when it exists)."""
    return feature_layers(project_root, app_name, name)[-1]


def expand_features(project_root: Path, app_name: str, features: list[str]) -> list[str]:
    out: list[str] = []
    for feature in features:
        out.extend(feature_layers(project_root, app_name, feature))
    return out


def append_local_overrides(
    project_root: Path, app_name: str, paths: list[str]
) -> list[str]:
    """Append app then shared local.overrides when those lab files exist.

    This is the uncommitted lab fragment (Wi-Fi and similar). It is not how
    application/<app>/configs and configs/ resolve the same CONFIG_* symbol.
    """
    out = list(paths)
    for rel in (
        f"application/{app_name}/{APP_SDK_REL}/local.overrides",
        f"{SHARED_DIR}/local.overrides",
    ):
        if (project_root / rel).is_file() and rel not in out:
            out.append(rel)
    return out


def app_feature_list(
    project_root: Path, app_name: str
) -> tuple[list[str], str]:
    """Return (features, board_manager_name) for the product's app.yml."""
    data = load_app_recipe(project_root, app_name)
    features = list(data.get("features") or [])
    board = str(data.get("board") or "").strip()
    if not board:
        raise ValueError(f"app.yml for {app_name!r} missing 'board' field")
    return features, board


def app_has_feature(project_root: Path, app_name: str, feature: str) -> bool:
    try:
        features, _ = app_feature_list(project_root, app_name)
    except FileNotFoundError:
        return False
    return feature in features

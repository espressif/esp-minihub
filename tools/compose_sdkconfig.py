# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Compose SDKCONFIG_DEFAULTS from shared + per-app sdkconfig fragments.

Product = application/<app>/ folder (MINIHUB_APP), e.g. minihub / claw_minihub.
Each product has one recipe: application/<app>/config/sdkconfig/app.yml
Optional HW recipes: variants/*.yml (copy over app.yml when needed).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None  # type: ignore

SHARED_DIR = "config/sdkconfig"
APP_SDK_REL = "config/sdkconfig"  # under application/<app>/
STACKS_GEN_REL = "cmake/generated/sdkconfig_stacks.gen"


def app_sdk_dir(project_root: Path, app_name: str) -> Path:
    return project_root / "application" / app_name / APP_SDK_REL


def shared_sdk_dir(project_root: Path) -> Path:
    return project_root / SHARED_DIR


def load_app_recipe(project_root: Path, app_name: str) -> dict[str, Any]:
    """Load application/<app>/config/sdkconfig/app.yml."""
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


def feature_to_path(project_root: Path, app_name: str, name: str) -> str:
    """Resolve feature id → repo-relative path (prefer app, then shared)."""
    if name in ("common", "memory"):
        return f"{SHARED_DIR}/{name}.defaults"
    if name in ("generated/stacks.gen", "stacks.gen", STACKS_GEN_REL):
        return STACKS_GEN_REL

    key = name
    for prefix in ("feature/", "stack/", "shared/"):
        if key.startswith(prefix):
            key = key[len(prefix) :]
            break

    app_rel = f"application/{app_name}/{APP_SDK_REL}/feature/{key}.defaults"
    shared_rel = f"{SHARED_DIR}/feature/{key}.defaults"
    if (project_root / app_rel).is_file():
        return app_rel
    if (project_root / shared_rel).is_file():
        return shared_rel
    raise FileNotFoundError(
        f"sdkconfig feature {key!r} not found for app={app_name!r} "
        f"(tried {app_rel} and {shared_rel})"
    )

def expand_features(project_root: Path, app_name: str, features: list[str]) -> list[str]:
    return [feature_to_path(project_root, app_name, f) for f in features]


def append_local_overrides(
    project_root: Path, app_name: str, paths: list[str]
) -> list[str]:
    """Append app then shared local.overrides when present."""
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

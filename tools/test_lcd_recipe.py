#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""LCD product build recipe: the symbols the UI cannot exist without.

application/minihub_lcd is the only LCD Host. application/minihub is Ethernet
Function CoreBoard only and must not carry an lcd fragment or korvo1 board.
"""

from __future__ import annotations

import re
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from compose_sdkconfig import expand_features, feature_layers  # noqa: E402

APP_ETH = "minihub"
APP_LCD = "minihub_lcd"
ETH_VARIANT_DIR = ROOT / "application" / APP_ETH / "configs" / "sdkconfig" / "variants"


def eth_variant_recipe(variant: str) -> dict:
    path = ETH_VARIANT_DIR / f"{variant}.yml"
    data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    if not isinstance(data, dict):
        raise AssertionError(f"{path} must be a mapping")
    return data


def composed(which: str) -> dict[str, str]:
    """Symbol table after stacking a recipe's feature fragments, last line wins."""
    if which == "lcd":
        app = APP_LCD
        recipe = ROOT / "application" / app / "configs" / "sdkconfig" / "app.yml"
        data = yaml.safe_load(recipe.read_text(encoding="utf-8")) or {}
        features = list(data["features"])
    else:
        app = APP_ETH
        features = list(eth_variant_recipe(which)["features"])
    out: dict[str, str] = {}
    for rel in expand_features(ROOT, app, features):
        for raw in (ROOT / rel).read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                out[line.split()[1]] = "n"
            elif line.startswith("CONFIG_") and "=" in line:
                key, val = line.split("=", 1)
                out[key] = val
    return out


class LcdRecipeTest(unittest.TestCase):
    def test_lcd_ui_is_enabled_by_its_own_recipe(self):
        self.assertEqual(composed("lcd").get("CONFIG_MINIHUB_LCD_UI"), "y")

    def test_memfs_is_given_a_drive_letter(self):
        table = composed("lcd")
        self.assertEqual(table.get("CONFIG_LV_USE_FS_MEMFS"), "y")
        self.assertEqual(
            table.get("CONFIG_LV_FS_MEMFS_LETTER"),
            "77",
            "LV_FS_MEMFS_LETTER defaults to 0 and lv_fs_memfs.c hard-errors on it",
        )

    def test_eth_product_has_no_lcd_variant(self):
        eth = yaml.safe_load(
            (ROOT / "application" / APP_ETH / "manifest.yml").read_text(encoding="utf-8")
        ) or {}
        self.assertNotIn("lcd", (eth.get("variants") or {}))
        self.assertFalse(
            (ROOT / "application" / APP_ETH / "boards" / "esp32_s31_korvo1").exists()
        )
        self.assertFalse(
            (ETH_VARIANT_DIR / "lcd.yml").exists()
        )


LCD_MANIFEST = ROOT / "application" / APP_LCD / "manifest.yml"


def lcd_product() -> dict:
    data = yaml.safe_load(LCD_MANIFEST.read_text(encoding="utf-8")) or {}
    if not isinstance(data, dict):
        raise AssertionError(f"{LCD_MANIFEST} must be a mapping")
    return data


class LcdManifestTest(unittest.TestCase):
    def test_hal_boards_is_not_a_dependency(self):
        """Owning the board definition replaces the hal_boards JSON catalog."""
        deps = lcd_product().get("idf_dependencies") or {}
        self.assertNotIn("espressif/brookesia_hal_boards", deps)
        manifests = (lcd_product().get("component_manifests") or {}).get("minihub_lcd") or {}
        self.assertNotIn("espressif/brookesia_hal_boards", manifests.get("dependencies") or {})

    def test_gui_lvgl_pinned_to_the_measured_floor(self):
        deps = lcd_product().get("idf_dependencies") or {}
        self.assertEqual(deps.get("espressif/brookesia_gui_lvgl"), ">=0.8.6,<0.9")
        self.assertEqual(deps.get("espressif/esp_lvgl_adapter"), "0.6.*")

    def test_sibling_brookesia_packages_stay_on_the_measured_08_series(self):
        """Only gui_lvgl is 0.8.6; pinning its siblings to >=0.8.6 breaks solving."""
        deps = lcd_product().get("idf_dependencies") or {}
        cm = (lcd_product().get("component_manifests") or {}).get("minihub_lcd") or {}
        sibling_deps = cm.get("dependencies") or {}
        siblings = (
            "espressif/brookesia_lib_utils",
            "espressif/brookesia_hal_adaptor",
            "espressif/brookesia_service_manager",
            "espressif/brookesia_service_helper",
            "espressif/brookesia_service_display",
            "espressif/brookesia_gui_interface",
        )
        for name in siblings:
            self.assertEqual(deps.get(name), "0.8.*", f"{name} in idf_dependencies")
            self.assertEqual(sibling_deps.get(name), "0.8.*", f"{name} in minihub_lcd manifest")

    def test_require_is_a_delta_not_a_copy(self):
        """deep_merge() already unions `require`, so the product lists only its own add."""
        import copy
        from sync_host_manifest import deep_merge, resolve_profile

        shared = yaml.safe_load(
            (ROOT / "configs" / "minihub_manifest.yml").read_text(encoding="utf-8")
        )
        base = resolve_profile(shared, "_host_base")
        merged = resolve_profile(copy.deepcopy(shared), "_host_base")
        merged = deep_merge(merged, lcd_product())
        got = merged["components"]["require"]
        want = base["components"]["require"] + ["minihub_lcd"]
        self.assertEqual(got, want)
        self.assertEqual(
            lcd_product()["components"]["require"],
            ["minihub_lcd"],
            "minihub_lcd.require must not re-list the base components",
        )

    def test_dead_lcd_ui_txt_is_not_written(self):
        import sync_host_manifest

        src = Path(sync_host_manifest.__file__).read_text(encoding="utf-8")
        self.assertNotIn('gen_dir / "lcd_ui.txt"', src)

    def test_hal_boards_is_not_a_cmake_require(self):
        src = (ROOT / "components" / "minihub_lcd" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("brookesia_hal_boards", src)


LCD_BOARD = lcd_product()["board"]
BOARD_DIR = ROOT / "application" / APP_LCD / "boards" / LCD_BOARD


def board_yaml(name: str):
    return yaml.safe_load((BOARD_DIR / name).read_text(encoding="utf-8")) or {}


def flatten(node):
    """Every dict reachable from a parsed YAML node."""
    if isinstance(node, dict):
        yield node
        for v in node.values():
            yield from flatten(v)
    elif isinstance(node, list):
        for v in node:
            yield from flatten(v)


class LcdBoardTest(unittest.TestCase):
    def test_product_owns_the_lcd_board(self):
        self.assertTrue(
            (BOARD_DIR / "board_info.yaml").is_file(),
            f"expected a product-local board override at {BOARD_DIR}",
        )
        self.assertEqual(LCD_BOARD, "esp32_s31_korvo1")

    def test_adc_peripheral_is_gone(self):
        """SOC_ADC_CHANNEL_NUM is missing from the pinned esp32s31 soc_caps.h."""
        periphs = board_yaml("board_peripherals.yaml").get("peripherals") or []
        self.assertNotIn("adc", [p.get("type") for p in periphs if isinstance(p, dict)])
        devs = board_yaml("board_devices.yaml").get("devices") or []
        self.assertNotIn("adc_button_group", [d.get("name") for d in devs])

    def test_camera_is_not_declared(self):
        devs = board_yaml("board_devices.yaml").get("devices") or []
        self.assertNotIn("camera", [d.get("name") for d in devs])
        defaults = (BOARD_DIR / "sdkconfig.defaults.board").read_text(encoding="utf-8")
        self.assertNotIn("CONFIG_CAMERA_OV3660", defaults)
        self.assertNotIn("CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE", defaults)

    def test_rgb_panel_uses_two_framebuffers(self):
        devs = {d["name"]: d for d in board_yaml("board_devices.yaml").get("devices") or []}
        panel = devs["display_lcd"]["config"]["rgb_panel_config"]
        self.assertEqual(panel["num_fbs"], 2)
        self.assertEqual(panel["timings"]["pclk_hz"], 26000000)
        self.assertTrue(panel["flags"]["fb_in_psram"])

    def test_gen_bmgr_staleness_uses_the_source_fingerprint(self):
        """The old CONFIG_SPIRAM probe is permanently true: board defaults carry no SPIRAM."""
        src = (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8")
        self.assertNotIn('"CONFIG_SPIRAM=y" not in defaults_text', src)
        assign = re.search(r"local_boards_stale\s*=\s*(.+)", src)
        self.assertIsNotNone(assign, "local_boards_stale assignment vanished")
        self.assertIn("customer", assign.group(1),
                      "staleness must key off the customer board, not a CONFIG_SPIRAM probe")


CI_FILE = ROOT / ".gitlab-ci.yml"


class LcdCiWiringTest(unittest.TestCase):
    def test_ci_builds_the_lcd_variant(self):
        ci = yaml.safe_load(CI_FILE.read_text(encoding="utf-8"))
        job = ci.get("build:app_lcd")
        self.assertIsInstance(job, dict, "no build:app_lcd job")
        script = "\n".join(job["script"])
        self.assertIn("MINIHUB_APP: minihub_lcd", yaml.safe_dump(job.get("variables") or {}))
        self.assertNotIn("apply_sdkconfig_variant.sh", script)
        self.assertIn("build_firmware.py", script)
        self.assertIn("build", script)
        self.assertEqual(job["stage"], "build")

    def test_ci_runs_the_lcd_contract_checks(self):
        ci = yaml.safe_load(CI_FILE.read_text(encoding="utf-8"))
        job = ci.get("check:lcd_contract")
        self.assertIsInstance(job, dict, "no check:lcd_contract job")
        script = "\n".join(job["script"])
        self.assertIn("tools/test_lcd_recipe.py", script)
        self.assertIn("tools/test_lcd_ui_contract.py", script)
        self.assertEqual(job["stage"], "pre_check")


LCD_PARTITIONS = ROOT / "application" / APP_LCD / "partitions.csv"


class LcdPartitionTest(unittest.TestCase):
    """application/minihub_lcd/partitions.csv is one 8 MiB OTA slot."""

    @staticmethod
    def rows():
        out = []
        for raw in LCD_PARTITIONS.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if line and not line.startswith("#"):
                out.append([c.strip() for c in line.split(",")])
        return out

    def test_lcd_recipe_keeps_the_product_table(self):
        self.assertEqual(
            composed("lcd").get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"),
            '"partitions.csv"')
        self.assertEqual(
            composed("default").get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"),
            '"partitions.csv"')

    def test_ota_slots_fit_the_image_and_the_layout_has_no_overlap(self):
        cursor = 0
        slots = []
        for name, _type, _sub, offset, size, *_ in self.rows():
            start = int(offset, 16) if offset else cursor
            length = int(size, 16)
            self.assertGreaterEqual(start, cursor, f"{name} overlaps the previous partition")
            cursor = start + length
            if _type == "app":
                slots.append(length)
        self.assertEqual(slots, [0x800000])
        self.assertEqual(cursor, 0xF00000)


IMPORT_CMAKE = ROOT / "cmake" / "minihub_bundle" / "minihub_import.cmake"


class MosquittoSysconfCompatTest(unittest.TestCase):
    """The LCD stack pulls libstdc++ <filesystem>, exposing a duplicate sysconf()."""

    def test_shim_renames_only_mosquitto_private_symbol(self):
        src = IMPORT_CMAKE.read_text(encoding="utf-8")
        self.assertIn("if(CONFIG_MINIHUB_MQTT_BROKER)", src)
        self.assertIn(
            'target_compile_definitions(${_mosq_lib} PRIVATE "sysconf=mosq_sysconf")', src)
        self.assertIn("idf_build_get_property(_minihub_build_components BUILD_COMPONENTS)", src)

    def test_shim_runs_after_project(self):
        """BUILD_COMPONENTS / CONFIG_* do not exist before project()."""
        src = IMPORT_CMAKE.read_text(encoding="utf-8")
        self.assertIn("cmake_language(DEFER DIRECTORY", src)
        self.assertIn("minihub_after_project", src)


LCD = ROOT / "components" / "minihub_lcd"


class FallbackCadenceTest(unittest.TestCase):
    def test_fallback_period_is_one_symbol_one_value(self):
        sys.path.insert(0, str(ROOT / "tools"))
        self.assertEqual(composed("lcd").get("CONFIG_MINIHUB_LCD_UI_REFRESH_MS"), "30000")

    def test_kconfig_help_says_what_the_number_is_now(self):
        src = (LCD / "Kconfig.minihub").read_text(encoding="utf-8")
        self.assertIn("default 30000", src)
        self.assertIn("fallback", src.lower())
        self.assertNotIn('int "LCD UI data refresh interval (ms)"', src)


STORE_C = ROOT / "components" / "minihub_store" / "minihub_store.c"


class StorageQuotaTest(unittest.TestCase):
    def test_quota_symbol_exists_once(self):
        kconfig = (LCD / "Kconfig.minihub").read_text(encoding="utf-8")
        self.assertIn("config MINIHUB_STORAGE_MIN_FREE_BYTES", kconfig)
        self.assertEqual(1, kconfig.count("config MINIHUB_STORAGE_MIN_FREE_BYTES"))

    def test_quota_value_is_in_the_recipe(self):
        self.assertEqual(
            composed("lcd").get("CONFIG_MINIHUB_STORAGE_MIN_FREE_BYTES"), "262144")

    def test_gate_sits_on_the_single_install_funnel(self):
        # Literal line fixed by the Wave B cross-lane contract (S writes it once).
        src = STORE_C.read_text(encoding="utf-8")
        self.assertIn("esp_err_t err = store_check_free_space(kind, id);", src)
        self.assertEqual(1, src.count("store_check_free_space(") - 1,
                         "the quota check belongs in store_install_from_catalog only")

    def test_gate_uses_the_partition_label_api(self):
        src = STORE_C.read_text(encoding="utf-8")
        self.assertIn('esp_littlefs_info("storage"', src)
        self.assertNotIn("esp_vfs_littlefs_info", src)


class FeatureSymbolMergeTest(unittest.TestCase):
    def test_product_symbol_overrides_shared_and_keeps_shared_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shared = root / "configs/sdkconfig/feature"
            app = root / "application/demo/configs/sdkconfig/feature"
            shared.mkdir(parents=True)
            app.mkdir(parents=True)
            (shared / "wifi_sta.defaults").write_text(
                "CONFIG_A=1\nCONFIG_B=shared\n", encoding="utf-8"
            )
            (app / "wifi_sta.defaults").write_text("CONFIG_B=app\n", encoding="utf-8")
            self.assertEqual(
                feature_layers(root, "demo", "wifi_sta"),
                [
                    "configs/sdkconfig/feature/wifi_sta.defaults",
                    "application/demo/configs/sdkconfig/feature/wifi_sta.defaults",
                ],
            )
            table: dict[str, str] = {}
            for rel in expand_features(root, "demo", ["wifi_sta"]):
                for raw in (root / rel).read_text(encoding="utf-8").splitlines():
                    key, val = raw.split("=", 1)
                    table[key] = val
            self.assertEqual(table["CONFIG_A"], "1")
            self.assertEqual(table["CONFIG_B"], "app")


if __name__ == "__main__":
    unittest.main()

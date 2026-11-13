#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""LCD JSON UI <-> ui_contract.json, both directions.

The C++ side used to hard-code ~40 absolute view paths and ~20 action names, and
the only check was one ESP_LOGW at runtime. This suite is the compile-time half:
a name the firmware reads must exist in the JSON the OEM edits, and a JSON file
that drops it must fail the build.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from test_lcd_recipe import composed, STORE_C  # noqa: E402  (one source of symbols)

LCD = ROOT / "components" / "minihub_lcd"
LCD_H = LCD / "include" / "minihub_lcd.h"
LCD_C = LCD / "minihub_lcd.c"
LCD_APP_CPP = LCD / "minihub_lcd_app.cpp"
LCD_OTA_C = ROOT / "components" / "minihub_ota" / "minihub_ota.c"
CONTRACT = LCD / "ui_contract.json"
UI = LCD / "ui"
GEN = ROOT / "tools" / "gen_lcd_ui_contract.py"


def load_contract() -> dict:
    return json.loads(CONTRACT.read_text(encoding="utf-8"))


def contract_root_assets() -> list[str]:
    return json.loads((UI / "root.json").read_text(encoding="utf-8"))["assets"]


def ui_assets() -> list[dict]:
    out = []
    for entry in contract_root_assets():
        path = UI / entry
        assert path.is_file(), f"root.json lists {entry} but it is missing"
        out.append(json.loads(path.read_text(encoding="utf-8")))
    return out


def collect_actions(node, actions: set[str]) -> None:
    """Collect event action names without adding any absolute node path.

    Template node ids are relative (icon/name/type/...) and must never leak
    into the concrete screen path set; only their action names are real.
    """
    if isinstance(node, dict):
        ev = node.get("events")
        if isinstance(ev, list):
            for e in ev:
                if isinstance(e, dict) and isinstance(e.get("action"), str):
                    actions.add(e["action"])
        for child in (node.get("children") or []):
            collect_actions(child, actions)
    elif isinstance(node, list):
        for item in node:
            collect_actions(item, actions)


def template_children(template_id: str) -> set[str]:
    """Ids declared inside one viewTemplate asset (relative ids, not paths)."""
    ids: set[str] = set()

    def walk(node):
        if isinstance(node, dict):
            ident = node.get("id")
            if isinstance(ident, str):
                ids.add(ident)
            for child in (node.get("children") or []):
                walk(child)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    for asset in ui_assets():
        if isinstance(asset, dict) and asset.get("type") == "viewTemplate" \
                and asset.get("id") == template_id:
            walk(asset.get("node") or {})
    return ids


def view_paths() -> tuple[set[str], set[str], set[str]]:
    """(concrete screen paths, template ids, action names) as the JSON offers them."""
    paths: set[str] = set()
    templates: set[str] = set()
    actions: set[str] = set()

    def walk(node, prefix):
        if isinstance(node, dict):
            ident = node.get("id")
            here = f"{prefix}/{ident}" if ident and prefix else prefix
            if ident and prefix:
                paths.add(here)
            ev = node.get("events")
            if isinstance(ev, list):
                for e in ev:
                    if isinstance(e, dict) and isinstance(e.get("action"), str):
                        actions.add(e["action"])
            for child in (node.get("children") or []):
                walk(child, here if (ident and prefix) else prefix)
        elif isinstance(node, list):
            for item in node:
                walk(item, prefix)

    for asset in ui_assets():
        if isinstance(asset, dict) and asset.get("type") == "screenFlow":
            for tr in (asset.get("transitions") or []):
                if isinstance(tr, dict) and isinstance(tr.get("action"), str):
                    actions.add(tr["action"])
        if isinstance(asset, dict) and asset.get("type") == "viewTemplate":
            templates.add(asset["id"])
            # Only the shared action is real; template node ids stay relative.
            collect_actions(asset.get("node") or {}, actions)
        if isinstance(asset, dict) and asset.get("type") == "viewScreen":
            walk({"children": asset.get("children") or []}, f"/{asset['id']}")
    return paths, templates, actions


def product(lists):
    if not lists:
        yield ()
        return
    head, *rest = lists
    for value in head:
        for tail in product(rest):
            yield (value, *tail)


def re_sub(fmt: str, args: list[str], table: dict) -> str:
    """Substitute each %d/%s by the ORIGINAL args position.

    A key repeated in args reuses the SAME value (no cartesian expansion); the
    cartesian product is only over distinct keys, per the contract rule.
    """
    result = ""
    i = 0
    idx = 0
    while i < len(fmt):
        if fmt[i] == "%" and i + 1 < len(fmt):
            result += str(table[args[idx]])
            idx += 1
            i += 2
        else:
            result += fmt[i]
            i += 1
    return result


def expand(fmt: str, args: list[str], contract: dict) -> list[str]:
    counts = contract["counts"]
    enums = contract["enums"]

    def candidates(key: str) -> list:
        if key in counts:
            return list(range(counts[key]))
        return list(enums[key])

    keys = list(dict.fromkeys(args))  # dedup: repeated keys reuse one value
    out = []
    for combo in product([candidates(k) for k in keys]):
        table = dict(zip(keys, combo))
        out.append(re_sub(fmt, args, table))
    return out


class ContractStructureTest(unittest.TestCase):
    def test_schema_and_root_version(self):
        c = load_contract()
        self.assertEqual(c["schema"], "minihub.lcd.ui_contract/1")
        root = json.loads((UI / "root.json").read_text(encoding="utf-8"))
        self.assertEqual(root["version"], c["root_version"])

    def test_flow_and_screens_agree(self):
        c = load_contract()
        flows = [a for a in ui_assets() if a.get("type") == "screenFlow"]
        self.assertEqual(len(flows), 1)
        self.assertEqual(flows[0]["id"], c["flow_id"])
        self.assertEqual(sorted(flows[0]["screens"]), sorted(c["enums"]["screens"]))

    def test_generator_output_is_current(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ui_contract.gen.h"
            subprocess.run(
                [sys.executable, str(GEN), "--out", str(out)],
                check=True, cwd=str(ROOT), capture_output=True, text=True,
            )
            text = out.read_text(encoding="utf-8")
        c = load_contract()
        self.assertIn("#define LCD_COUNT_DEVICE_SLOTS 12", text)
        for key, val in c["views"].items():
            self.assertIn(f'#define LCD_VIEW_{key} "{val}"', text)

    def test_binding_keys_match_what_the_json_declares(self):
        """A binding the C++ writes but no node declares is applied nowhere: the
        runtime only logs "no binding declaration matched key". Keep the
        contract's list and the JSON's declarations the same set."""
        declared = set()
        for entry in contract_root_assets():
            asset = json.loads((UI / entry).read_text(encoding="utf-8"))

            def walk(node):
                if isinstance(node, dict):
                    for value in (node.get("bindings") or {}).values():
                        if isinstance(value, str):
                            declared.add(value)
                    for child in (node.get("children") or []):
                        walk(child)
                    if isinstance(node.get("node"), dict):
                        walk(node["node"])
                elif isinstance(node, list):
                    for item in node:
                        walk(item)

            walk(asset)
        listed = set(load_contract()["binding_keys"])
        self.assertEqual(declared, listed,
                         f"undeclared in contract: {sorted(declared - listed)}; "
                         f"stale in contract: {sorted(listed - declared)}")


class ContractCoverageTest(unittest.TestCase):
    def test_every_declared_view_exists(self):
        paths, _, _ = view_paths()
        c = load_contract()
        for key, path in c["views"].items():
            self.assertIn(path, paths, f"view {key} is not in the JSON UI")

    def test_every_expanded_pattern_exists(self):
        paths, _, _ = view_paths()
        c = load_contract()
        for key, spec in c["view_patterns"].items():
            for made in expand(spec["format"], spec["args"], c):
                self.assertIn(made, paths, f"pattern {key} expands to a missing view: {made}")

    def test_every_expanded_action_exists(self):
        _, _, actions = view_paths()
        c = load_contract()
        for key, name in c["actions"].items():
            self.assertIn(name, actions, f"action {key} is never offered by the UI")
        for key, spec in c["action_patterns"].items():
            for made in expand(spec["format"], spec["args"], c):
                self.assertIn(made, actions, f"action pattern {key}: {made} missing")

    def test_counts_match_the_json_not_the_other_way_round(self):
        """12 slots is a claim about the device-card template, not a magic 12.

        Task 8 replaced the 12 pre-instantiated dev_N subtrees with one template
        plus runtime create_view(), so the evidence moved from overview.json to
        the contract + generated header + template asset.
        """
        c = load_contract()
        spec = c["templates"]["DEVICE_CARD"]
        self.assertEqual(spec["count_key"], "device_slots")
        self.assertEqual(c["counts"][spec["count_key"]], c["counts"]["device_slots"])
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ui_contract.gen.h"
            subprocess.run(
                [sys.executable, str(GEN), "--out", str(out)],
                check=True, cwd=str(ROOT), capture_output=True, text=True,
            )
            text = out.read_text(encoding="utf-8")
        self.assertIn(
            f'#define LCD_COUNT_DEVICE_SLOTS {c["counts"]["device_slots"]}', text)
        self.assertIn(
            f'#define LCD_TEMPLATE_DEVICE_CARD_COUNT {c["counts"]["device_slots"]}', text)
        ids = template_children(spec["template_id"])
        self.assertTrue(set(spec["children"]).issubset(ids),
                        f"template is missing children: {set(spec['children']) - ids}")


CPP_FILES = [LCD / "minihub_lcd_app.cpp", LCD / "minihub_lcd_module_auth.cpp"]


class NoBareLiteralsTest(unittest.TestCase):
    def test_app_includes_the_generated_header(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "ui_contract.gen.h"', src)

    def test_no_bare_view_paths_left_in_cpp(self):
        for f in CPP_FILES:
            src = f.read_text(encoding="utf-8")
            offenders = [
                line.strip() for line in src.splitlines()
                if ('"/overview' in line or '"/store' in line or '"/map/' in line)
                and "LCD_VIEW" not in line
            ]
            self.assertEqual([], offenders, f"{f.name} still hard-codes view paths")

    def test_no_bare_action_names_left_in_cpp(self):
        for f in CPP_FILES:
            src = f.read_text(encoding="utf-8")
            offenders = [
                line.strip() for line in src.splitlines()
                if ('"nav_' in line or '"filter_' in line or '"auth_' in line
                    or '"user_tap' in line or '"device_tap' in line)
                and "LCD_ACTION" not in line
            ]
            self.assertEqual([], offenders, f"{f.name} still hard-codes action names")

    def test_slot_count_comes_from_the_contract(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "constexpr int kOverviewDeviceSlots = LCD_COUNT_DEVICE_SLOTS;", src)
        self.assertNotIn("constexpr int kOverviewDeviceSlots = 12;", src)


class DeviceTemplateTest(unittest.TestCase):
    def test_contract_declares_the_device_card_template(self):
        c = load_contract()
        spec = c["templates"]["DEVICE_CARD"]
        self.assertEqual(spec["template_id"], "device_card")
        self.assertEqual(spec["parent"], "/overview/overview_main/device_list")
        self.assertEqual(
            spec["children"],
            ["icon", "name", "type", "meta", "val", "rename_btn"])
        self.assertNotIn("DEVICE_CARD", c["view_patterns"])

    def test_template_exists_and_has_no_indexed_slots(self):
        _, templates, actions = view_paths()
        self.assertIn("device_card", templates)
        paths, _, _ = view_paths()
        stale = [p for p in paths if "/device_list/dev_" in p]
        self.assertEqual([], stale, "overview.json still ships pre-instantiated dev_N cards")

    def test_one_action_covers_every_instance(self):
        _, _, actions = view_paths()
        c = load_contract()
        self.assertIn("device_tap", actions, "the template must offer one shared action")
        self.assertNotIn("device_tap_0", actions)
        self.assertEqual("device_tap", c["actions"]["DEVICE_TAP"])
        # A template may share one action across its instances: every instance
        # gets its own scope at runtime, so the loader sees one owner. Only
        # hand-written nodes need unique names, and those are declared as
        # action_patterns -- a template action must stay a plain action.
        patterns = c.get("action_patterns") or {}
        for key in ("DEVICE_TAP", "RULE_TOGGLE", "SCENE_INVOKE"):
            self.assertNotIn(key, patterns,
                             f"{key} is a template action; a pattern would give "
                             "every instance its own name for nothing")

    def test_cpp_instantiates_from_the_template(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn("LCD_TEMPLATE_DEVICE_CARD_ID", src)
        self.assertIn("->create_view(", src)
        self.assertIn("->destroy_view(", src)
        self.assertIn("ev.path", src)
        self.assertIn("LCD_ACTION_DEVICE_TAP", src)
        self.assertNotIn("device_tap_%d", src)

    def test_instance_prefix_and_format_agree(self):
        spec = load_contract()["templates"]["DEVICE_CARD"]
        self.assertEqual("dev_", spec["instance_format"].split("%d")[0])
        self.assertEqual(spec["instance_prefix"], spec["instance_format"].split("%d")[0])


class RefreshWiringTest(unittest.TestCase):
    def test_lcd_subscribes_to_the_point_events(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        for event in ("point.updated", "point.registered", "point.unregistered"):
            self.assertIn(f'minihub_event_bus_subscribe("{event}"', src, event)

    def test_lcd_unsubscribes_on_teardown(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        for event in ("point.updated", "point.registered", "point.unregistered"):
            self.assertIn(f'minihub_event_bus_unsubscribe("{event}"', src, event)

    def test_events_are_coalesced_not_replayed_per_point(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn("constexpr int kRefreshCoalesceMs = 100;", src)

    def test_no_unconditional_full_poll_loop(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertNotIn("post_delayed([]() { refresh_task(); }, CONFIG_MINIHUB_LCD_UI_REFRESH_MS)", src)
        self.assertIn("s_events_live", src)

    def test_only_the_visible_screen_is_rebuilt(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn("void schedule_refresh()", src)
        self.assertLess(
            src.index("void schedule_refresh()"),
            src.index("void refresh_task()"),
            "schedule_refresh must exist before its first caller",
        )


class FontContractTest(unittest.TestCase):
    def test_builtin_cjk_is_on_in_the_recipe(self):
        table = composed("lcd")
        self.assertEqual(table.get("CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK"), "y")

    def test_native_font_declared_and_registered(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn("LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk)", src)
        self.assertIn('"cjk_16"', src)
        self.assertIn("&lv_font_source_han_sans_sc_16_cjk", src)

    def test_missing_otf_falls_back_to_builtin_cjk(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        self.assertIn('const char *fallback = "cjk_16";', src)

    def test_zh_default_font_never_uses_latin_only(self):
        src = (LCD / "minihub_lcd_app.cpp").read_text(encoding="utf-8")
        block = src[src.index("void bind_ui_font_language("):]
        block = block[:block.index("\n}")]
        self.assertNotIn('set_default_font_for_language("zh", "latin_14")', block)
        self.assertNotIn('const char *font_id = "latin_14";', block)


class FreezeWindowTest(unittest.TestCase):
    def test_hook_pair_is_declared(self):
        h = LCD_H.read_text(encoding="utf-8")
        self.assertIn("void minihub_lcd_flash_write_begin(void);", h)
        self.assertIn("void minihub_lcd_flash_write_end(void);", h)

    def test_stub_exists_for_the_disabled_build(self):
        c = LCD_C.read_text(encoding="utf-8")
        self.assertIn("#if !CONFIG_MINIHUB_LCD_UI", c)
        self.assertIn("void minihub_lcd_flash_write_begin(void)", c)
        self.assertIn("void minihub_lcd_flash_write_end(void)", c)

    def test_store_freeze_hooks_are_balanced(self):
        src = STORE_C.read_text(encoding="utf-8")
        self.assertEqual(src.count("minihub_lcd_flash_write_begin("),
                         src.count("minihub_lcd_flash_write_end("),
                         "every opened freeze window must be closed")

    def test_install_funnel_freezes_the_panel(self):
        src = STORE_C.read_text(encoding="utf-8")
        self.assertTrue("minihub_lcd_flash_write_begin();\n    err = store_install_item(" in src,
                        "the install funnel must open the freeze window")

    def test_uninstall_freezes_across_all_its_early_returns(self):
        """uninstall also writes littlefs, and its body has 7+ early returns.
        A bare begin/end pair around that body would leak the window open, so
        the public entry must be a single-exit wrapper over a static helper."""
        src = STORE_C.read_text(encoding="utf-8")
        self.assertTrue("static esp_err_t store_uninstall_item(" in src,
                        "uninstall body must move behind a static helper")
        # rindex: the file also carries a !DYNAMIC_MODULES stub of this name.
        wrap = src[src.rindex("esp_err_t minihub_store_uninstall("):]
        wrap = wrap[:wrap.index("\n}")]
        self.assertTrue("minihub_lcd_flash_write_begin();" in wrap, "uninstall wrapper is missing " + "minihub_lcd_flash_write_begin();")
        self.assertTrue("store_uninstall_item(kind, id)" in wrap, "uninstall wrapper is missing " + "store_uninstall_item(kind, id)")
        self.assertTrue("minihub_lcd_flash_write_end();" in wrap, "uninstall wrapper is missing " + "minihub_lcd_flash_write_end();")
        self.assertEqual(1, wrap.count("return "),
                         "the wrapper needs one exit so the window always closes")

    def test_lcd_defers_refresh_while_a_write_is_in_flight(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertIn("std::atomic<bool> s_flash_write{false};", src)
        self.assertIn("if (s_flash_write.load(std::memory_order_acquire))", src)

    def test_ota_window_is_observed_not_reimplemented(self):
        # Real symbol (plan :2385 wrote "miniota_is_busy()" by mistake).
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertIn("minihub_ota_is_busy()", src)
        ota = LCD_OTA_C.read_text(encoding="utf-8")
        self.assertNotIn("minihub_lcd_flash_write_begin", ota,
                         "OTA already exposes is_busy(); do not add a second signal")


def screen_asset(screen_id: str) -> dict:
    for asset in ui_assets():
        if isinstance(asset, dict) and asset.get("type") == "viewScreen" \
                and asset.get("id") == screen_id:
            return asset
    raise AssertionError(f"no viewScreen asset named {screen_id}")


def find_node(node, ident: str):
    """First node with this id anywhere under node (dict or list)."""
    if isinstance(node, dict):
        if node.get("id") == ident:
            return node
        for child in (node.get("children") or []):
            hit = find_node(child, ident)
            if hit is not None:
                return hit
    elif isinstance(node, list):
        for item in node:
            hit = find_node(item, ident)
            if hit is not None:
                return hit
    return None


def node_actions(node) -> list[str]:
    return [e["action"] for e in (node.get("events") or [])
            if isinstance(e, dict) and isinstance(e.get("action"), str)]


class StoreActionsTest(unittest.TestCase):
    """Row tap -> choice sheet -> install/upgrade/reinstall, uninstall confirmed.

    Mirrors the Web console: a row tap opens the choice sheet and the row
    identity is read back from Event.path (the device_tap / rule_toggle
    precedent), and builtin items (managed == false) get no operations at all.

    The tap action itself is *not* shared: the loader gives one owner per
    action name and rejects the whole document when two hand-written nodes
    share one, so each of the twelve rows owns its own name and the firmware
    subscribes to all twelve.
    """

    ACTIONS = {
        "STORE_INSTALL": "store_install",
        "STORE_UPGRADE": "store_upgrade",
        "STORE_REINSTALL": "store_reinstall",
        "STORE_ENABLE": "store_enable",
        "STORE_UNINSTALL": "store_uninstall",
        "STORE_MODAL_CANCEL": "store_modal_cancel",
    }
    VIEWS = {
        "STORE_MODAL": "/store/store_modal",
        "STORE_MODAL_TITLE": "/store/store_modal/store_modal_card/store_modal_title",
        "STORE_MODAL_MSG": "/store/store_modal/store_modal_card/store_modal_msg",
        "STORE_MODAL_INSTALL": "/store/store_modal/store_modal_card/store_modal_btns/store_do_install",
        "STORE_MODAL_UPGRADE": "/store/store_modal/store_modal_card/store_modal_btns/store_do_upgrade",
        "STORE_MODAL_REINSTALL": "/store/store_modal/store_modal_card/store_modal_btns/store_do_reinstall",
        "STORE_MODAL_ENABLE": "/store/store_modal/store_modal_card/store_modal_btns/store_do_enable",
        "STORE_MODAL_UNINSTALL": "/store/store_modal/store_modal_card/store_modal_btns/store_do_uninstall",
    }

    def test_contract_declares_the_store_actions(self):
        actions = load_contract()["actions"]
        for key, name in self.ACTIONS.items():
            self.assertIn(key, actions, f"contract is missing action {key}")
            self.assertEqual(name, actions[key])

    def test_contract_declares_the_store_row_tap_pattern(self):
        c = load_contract()
        patterns = c["action_patterns"]
        self.assertIn("STORE_ROW_TAP", patterns)
        spec = patterns["STORE_ROW_TAP"]
        self.assertEqual("store_row_tap_%s_%d", spec["format"])
        self.assertEqual(["store_row_prefixes", "store_row_slots"], spec["args"])
        self.assertNotIn("STORE_ROW_TAP", c["actions"],
                         "the shared row action is exactly what the loader rejects")

    def test_contract_declares_the_choice_sheet_views(self):
        views = load_contract()["views"]
        for key, path in self.VIEWS.items():
            self.assertIn(key, views, f"contract is missing view {key}")
            self.assertEqual(path, views[key])

    def test_every_store_row_owns_a_unique_tap_action(self):
        store = screen_asset("store")
        c = load_contract()
        prefixes = c["enums"]["store_row_prefixes"]
        slots = c["counts"]["store_row_slots"]
        seen = []
        for prefix in prefixes:
            for i in range(slots):
                ident = f"{prefix}_row_{i}"
                node = find_node(store, ident)
                self.assertIsNotNone(node, f"store.json has no row {ident}")
                expected = f"store_row_tap_{prefix}_{i}"
                self.assertEqual([expected], node_actions(node),
                                 f"row {ident} must own exactly {expected}")
                seen.append(expected)
        self.assertEqual(len(seen), len(set(seen)),
                         "every row needs its own action name")

    def test_choice_sheet_is_a_hidden_sibling_of_store_main(self):
        store = screen_asset("store")
        top_ids = [n.get("id") for n in (store.get("children") or [])
                   if isinstance(n, dict)]
        self.assertIn("store_modal", top_ids,
                      "the sheet must overlay store_main, not sit inside it")
        modal = find_node(store, "store_modal")
        self.assertTrue((modal.get("commonProps") or {}).get("hidden"),
                        "the sheet must start hidden")
        for ident, action in (("store_do_install", "store_install"),
                              ("store_do_upgrade", "store_upgrade"),
                              ("store_do_enable", "store_enable"),
                              ("store_do_reinstall", "store_reinstall"),
                              ("store_do_uninstall", "store_uninstall"),
                              ("store_modal_cancel", "store_modal_cancel")):
            node = find_node(modal, ident)
            self.assertIsNotNone(node, f"sheet has no button {ident}")
            self.assertEqual([action], node_actions(node))

    def test_cpp_reads_the_row_index_from_the_event_path(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("LCD_ACTION_PATTERN_STORE_ROW_TAP" in src,
                        "C++ is missing LCD_ACTION_PATTERN_STORE_ROW_TAP")
        self.assertNotIn("LCD_ACTION_STORE_ROW_TAP", src,
                         "the shared row action is gone; the pattern replaced it")
        for macro in ("LCD_VIEW_STORE_MODAL", "LCD_VIEW_STORE_MODAL_TITLE",
                      "LCD_VIEW_STORE_MODAL_MSG", "LCD_VIEW_STORE_MODAL_INSTALL",
                      "LCD_VIEW_STORE_MODAL_UNINSTALL"):
            self.assertTrue(macro in src, f"C++ does not use {macro}")

    def test_cpp_calls_all_four_store_operations(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        for fn in ("minihub_store_install(", "minihub_store_upgrade(",
                   "minihub_store_reinstall(", "minihub_store_uninstall("):
            self.assertTrue(fn in src, f"C++ never calls {fn}")

    def test_store_work_runs_off_the_ui_worker(self):
        """minihub_store_* downloads and extracts; on the UI worker it freezes
        every other LCD caller, the same starvation the JS worker rules forbid."""
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("std::atomic<bool> s_store_busy{false};" in src, "C++ is missing std::atomic<bool> s_store_busy{false};")
        self.assertTrue("void store_op_task(void *arg)" in src, "C++ is missing void store_op_task(void *arg)")
        body = src[src.index("void store_op_task"):]
        body = body[:body.index("\n}")]
        self.assertIn("minihub_lcd_request_refresh();", body,
                      "the worker must ask for a refresh when the operation finishes")
        self.assertTrue("xTaskCreate(" in src, "the operation must be spawned, not run inline")

    def test_only_uninstall_is_confirmed(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("StoreUninstall" in src, "C++ is missing StoreUninstall")
        ok = src[src.index("void handle_confirm_ok()"):]
        ok = ok[:ok.index("\n}")]
        self.assertIn("StoreUninstall", ok, "confirm OK must dispatch the uninstall")
        for name in ("StoreInstall", "StoreUpgrade", "StoreReinstall"):
            self.assertNotIn(name, ok, f"{name} must run without a second confirmation")

    def test_builtin_items_are_not_offered_for_uninstall(self):
        """Web: managed === false renders a 'builtin' label and no buttons."""
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        for field in ('"managed"', '"config_only"', '"upgrade_available"', '"binary_stale"'):
            self.assertTrue(field in src, f"C++ never reads the catalog field {field}")

    def test_uninstall_reloads_the_hub(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("minihub_hub_reload_async()" in src,
                        "the loader keeps a deleted slot until the hub rebuilds it")

    def test_confirm_modal_exists_on_every_screen_that_can_raise_one(self):
        """The modal is a child of its screen. Unhiding a descendant does not
        unhide the ancestor, so a confirm raised from the store screen needs a
        modal on the store screen — /system/confirm_modal is invisible there."""
        c = load_contract()
        self.assertIn("confirm_screens", c["enums"])
        self.assertEqual(["rules", "scenes", "store", "system"], sorted(c["enums"]["confirm_screens"]))
        for key in ("CONFIRM_MODAL", "CONFIRM_TITLE", "CONFIRM_MSG"):
            self.assertIn(key, c["view_patterns"], f"contract is missing pattern {key}")
            self.assertEqual(["confirm_screens"], c["view_patterns"][key]["args"])

    def test_show_confirm_targets_the_active_screen(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("LCD_VIEW_PATTERN_CONFIRM_MODAL" in src,
                        "show_confirm must resolve the modal on the active screen")
        self.assertNotIn('"/system/confirm_modal', src,
                         "a hard-coded system-screen modal is invisible from the store")

    def test_refresh_keeps_the_tab_the_user_selected(self):
        """refresh_store runs on every point event now. Forcing the 可安装 tab
        there makes 已安装 — where uninstall/upgrade live — impossible to stay on."""
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        body = src[src.index("void refresh_store()"):]
        body = body[:body.index("\n}")]
        self.assertFalse("store_set_tab(true);" in body,
                         "a refresh must re-apply the current tab, not reset it")
        self.assertTrue("store_set_tab(s_store_tab_available);" in body,
                        "refresh must re-apply the remembered tab")
        tab = src[src.index("void store_set_tab(bool available)"):]
        tab = tab[:tab.index("\n}")]
        self.assertTrue("s_store_tab_available = available;" in tab,
                        "store_set_tab must remember the choice")

    def test_sheet_acts_on_the_identity_captured_when_it_opened(self):
        """The row cache is rebuilt by every refresh, so an index held while the
        sheet is open can end up pointing at a different module. Capture the row
        when the sheet opens and act on that snapshot."""
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("StoreRowInfo s_store_selected{};" in src,
                        "the sheet needs a snapshot of the row it was opened on")
        show = src[src.index("void store_sheet_show("):]
        show = show[:show.index("\n}")]
        self.assertTrue("s_store_selected = info;" in show,
                        "store_sheet_show must capture the row")
        run = src[src.index("void store_sheet_run("):]
        run = run[:run.index("\n}")]
        self.assertFalse("s_store_index" in run,
                         "the button must not re-resolve a live row index")
        self.assertTrue("s_store_selected" in run, "the button must act on the snapshot")


class TimezonePickerTest(unittest.TestCase):
    """Enum picker, no keyboard: options come from the Core API, not a table here."""

    def test_contract_declares_the_dropdown_and_action(self):
        c = load_contract()
        self.assertIn("SYSTEM_TZ", c["views"])
        self.assertEqual("/system/system_main/sys_grid/sys_tz_row/sys_tz_val",
                         c["views"]["SYSTEM_TZ"])
        self.assertIn("SYSTEM_TZ_PICK", c["actions"])
        self.assertEqual("system_tz_pick", c["actions"]["SYSTEM_TZ_PICK"])

    def test_tz_and_lang_rows_sit_after_modstore_before_wifi(self):
        """User asked for tz/lang AFTER 模块仓库 (and before 连接状态)."""
        system = screen_asset("system")
        main = next(n for n in system["children"] if n["id"] == "system_main")
        grid = next(n for n in main["children"] if n["id"] == "sys_grid")
        ids = [n["id"] for n in grid["children"]]
        self.assertIn("sys_tz_row", ids)
        self.assertIn("sys_lang_row", ids)
        self.assertLess(ids.index("sys_card_modstore"), ids.index("sys_tz_row"))
        self.assertLess(ids.index("sys_lang_row"), ids.index("sys_card_wifi"))

    def test_system_json_uses_a_dropdown_node(self):
        system = screen_asset("system")
        node = find_node(system, "sys_tz_val")
        self.assertIsNotNone(node, "system.json has no sys_tz_val")
        self.assertEqual("dropdown", node.get("type"))
        events = node.get("events") or []
        self.assertEqual([{"type": "valueChanged", "action": "system_tz_pick"}], events)

    def test_options_come_from_the_core_api(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertTrue("minihub_tz_status_json()" in src, "C++ is missing minihub_tz_status_json()")
        self.assertTrue('"tz.options"' in src, "options must be bound at runtime")
        self.assertTrue("minihub_tz_set(" in src, "C++ is missing minihub_tz_set(")
        self.assertTrue("LCD_ACTION_SYSTEM_TZ_PICK" in src, "C++ is missing LCD_ACTION_SYSTEM_TZ_PICK")

    def test_no_hardcoded_zone_table_in_the_lcd(self):
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        self.assertNotIn('"UTC+8"', src,
                         "the zone list belongs to minihub_tz.c, not to the UI")

    def test_dropdown_declares_the_bindings_it_is_driven_through(self):
        """set_binding_value only applies through a declared `bindings` entry.
        With no declaration the runtime logs "no binding declaration matched
        key" and applies nothing, so the dropdown would keep its placeholder."""
        c = load_contract()
        node = find_node(screen_asset("system"), "sys_tz_val")
        bindings = node.get("bindings") or {}
        self.assertIn("dropdownProps.options", bindings)
        self.assertIn("dropdownProps.selectedIndex", bindings)
        for key in bindings.values():
            self.assertIn(key, c["binding_keys"],
                          f"binding key {key} is not declared in the contract")
        src = LCD_APP_CPP.read_text(encoding="utf-8")
        for key in bindings.values():
            self.assertTrue(f'"{key}"' in src, f"C++ never writes binding key {key}")


class CommonPropsSchemaTest(unittest.TestCase):
    """A commonProps key written at node top level is silently dropped.

    brookesia reads these ONLY from commonProps (parser_node.cpp:868-919) and
    CommonProps.hidden defaults to false (document.hpp:87). There is no
    unknown-field rejection, so `"hidden": true` on a node parses clean, mounts
    the node visible, and logs nothing. system.json's confirm_modal shipped
    exactly that: a full-screen opaque overlay covering the system screen from
    the moment it mounted.
    """

    COMMON_ONLY = ("hidden", "disabled", "clickable", "scrollable",
                   "pressLock", "press_lock", "angle", "zoom",
                   "pivotX", "pivot_x", "pivotY", "pivot_y")

    def test_no_common_props_key_sits_at_node_top_level(self):
        offenders = []

        def walk(node, where):
            if isinstance(node, dict):
                here = f"{where}/{node['id']}" if node.get("id") else where
                for key in self.COMMON_ONLY:
                    if key in node:
                        offenders.append(f"{here}: top-level '{key}' (ignored by the parser)")
                # Only descend into node subtrees. bindings/style/*Props are
                # property maps, and a binding key may legitimately be
                # "commonProps.hidden".
                for child in (node.get("children") or []):
                    walk(child, here)
                if isinstance(node.get("node"), dict):
                    walk(node["node"], here)
            elif isinstance(node, list):
                for item in node:
                    walk(item, where)

        for entry in contract_root_assets():
            path = UI / entry
            walk(json.loads(path.read_text(encoding="utf-8")), entry)
        self.assertEqual([], offenders,
                         "move these into commonProps; the parser drops them silently")


class ActionUniquenessTest(unittest.TestCase):
    """One action name, one owner.

    Brookesia refuses to load a document in which the same action name is
    owned by two different nodes: `validator.cpp:564` records the owner of an
    action and fails with "Document action must be unique" as soon as a second
    owner shows up. The owner is the node's view path (`make_action_owner()`,
    validator.cpp:315), which is why a hand-written node may not share an
    action with another hand-written node -- but a *template* may, because every
    instance of it gets its own scope at runtime.

    This was found on the device, not in the test suite: the store screen
    declared one `store_row_tap` on twelve static rows and the whole UI failed
    to load ("parse UI failed: Document action must be unique"), while every
    static gate stayed green because the actions were collected into a set.
    """

    def owners(self) -> dict[str, set[str]]:
        found: dict[str, set[str]] = {}

        def walk(node, rel: str, prefix: str) -> None:
            if isinstance(node, dict):
                ident = node.get("id")
                here = f"{rel}/{ident}" if ident else rel
                for e in (node.get("events") or []):
                    if isinstance(e, dict) and isinstance(e.get("action"), str):
                        act = e["action"]
                        owner = f"{prefix}{here}" if prefix else here
                        found.setdefault(act, set()).add(owner)
                for child in (node.get("children") or []):
                    walk(child, here, prefix)
            elif isinstance(node, list):
                for item in node:
                    walk(item, rel, prefix)

        for entry in contract_root_assets():
            path = UI / entry
            if not path.is_file():
                continue
            asset = json.loads(path.read_text(encoding="utf-8"))
            if asset.get("type") == "viewTemplate":
                # A template is declared once; each instance gets its own scope.
                walk(asset.get("node") or {}, "", "")
            elif asset.get("type") == "viewScreen":
                walk(asset, "", f"{asset.get('id')}/")
        return found

    def test_no_action_is_owned_by_two_different_nodes(self):
        offenders = []
        for action, owners in sorted(self.owners().items()):
            if len(owners) > 1:
                offenders.append(f"{action} -> {sorted(owners)}")
        self.assertEqual(
            [], offenders,
            "one action name may own only one node; the loader rejects the whole "
            "document otherwise (brookesia validator.cpp:564)")

    def test_the_shared_row_action_does_not_come_back(self):
        """Regression guard for the exact shape that broke the device."""
        owners = self.owners()
        self.assertNotIn("store_row_tap", owners)
        rows = [a for a in owners if a.startswith("store_row_tap_")]
        self.assertEqual(12, len(rows), "the twelve store rows each own one action")
        for action in rows:
            self.assertEqual(1, len(owners[action]), f"{action} owns {owners[action]}")


class LcdI18nTest(unittest.TestCase):
    """ui/i18n/strings.json is the single source for JSON constants and lcd_tr()."""

    def test_generated_language_assets_are_current(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "gen_lcd_i18n.py"), "--check"],
            cwd=str(ROOT), capture_output=True, text=True)
        self.assertEqual(
            0, result.returncode,
            "run tools/gen_lcd_i18n.py to refresh the language assets: "
            + result.stdout + result.stderr)

    def test_every_string_reference_has_a_dictionary_key(self):
        strings = json.loads(
            (UI / "i18n" / "strings.json").read_text(encoding="utf-8"))["strings"]
        refs = set()
        for path in UI.rglob("*.json"):
            rel = path.relative_to(UI).as_posix()
            if rel == "i18n/strings.json" or rel.startswith("constants/i18n_"):
                continue
            refs.update(re.findall(
                r"\$\{constant\.strings\.([a-z0-9_]+)\}",
                path.read_text(encoding="utf-8")))
        missing = sorted(refs - set(strings))
        self.assertEqual([], missing,
                         "UI JSON references undefined i18n keys: " + repr(missing))
        self.assertTrue(refs, "no ${constant.strings.*} references found")


if __name__ == "__main__":
    unittest.main()

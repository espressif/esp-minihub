#!/usr/bin/env python3
"""Assert miot_sync.c profile_for_model heuristics and Spec slim profile parse."""

from __future__ import annotations

import json
import sys
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tools"))
from store_server import slim_profile_from_instance  # noqa: E402


@dataclass(frozen=True)
class MiotProfile:
    type: str
    siid: int
    piid_on: int
    piid_bright: int = 0


def profile_for_model(model: str | None) -> MiotProfile:
    """Mirror of profile_for_model() match order in miot_sync.c."""
    p = MiotProfile(type="switch", siid=2, piid_on=1)
    if not model:
        return p

    if (
        "sensor_magnet" in model
        or ".magnet." in model
        or ("magnet" in model and "vacuum" not in model)
    ):
        piid_on = 2 if "isa.magnet" in model else 1
        return MiotProfile(type="binary_sensor", siid=2, piid_on=piid_on)

    if "sensor_smoke" in model or "smoke" in model:
        return MiotProfile(type="smoke", siid=2, piid_on=1)

    if model.startswith("micar.car.") or ".car." in model:
        return MiotProfile(type="car", siid=0, piid_on=0)

    if "router" in model:
        return MiotProfile(type="router", siid=3, piid_on=3)

    if ".light." in model or "lamp" in model or "ceiling" in model:
        return MiotProfile(type="light", siid=2, piid_on=1, piid_bright=2)

    if "sensor" in model or ".ht." in model or "temp" in model:
        return MiotProfile(type="sensor", siid=3, piid_on=7)

    if "curtain" in model or "airer" in model:
        return MiotProfile(type="cover", siid=2, piid_on=1)

    if "plug" in model or "switch" in model or "outlet" in model:
        return MiotProfile(type="switch", siid=2, piid_on=1)

    return p


SPEC_PROFILE_CASES = [
    {
        "model": "qdhkl.aircondition.ac",
        "urn": "urn:miot-spec-v2:device:air-conditioner:0000A004:qdhkl-ac:1",
        "expect": {
            "piid_on": 1,
            "piid_mode": 2,
            "piid_target_temp": 4,
            "siid_fan": 3,
            "piid_fan": 2,
            "siid_env": 4,
            "piid_temp": 7,
        },
    },
    {
        "model": "babai.curtain.bb82cb",
        "urn": "urn:miot-spec-v2:device:curtain:0000A00C:babai-bb82cb:1",
        "expect": {
            "piid_on": 0,
            "piid_position": 5,
            "motor_open": 1,
            "motor_close": 2,
        },
    },
    {
        "model": "novo.curtain.n21",
        "urn": "urn:miot-spec-v2:device:curtain:0000A00C:novo-n21:1",
        "expect": {
            "piid_on": 0,
            "piid_position": 6,
            "motor_open": 2,
            "motor_close": 1,
        },
    },
    {
        "model": "isa.camera.hlzoom",
        "urn": "urn:miot-spec-v2:device:camera:0000A01C:isa-hlzoom:1",
        "expect": {"siid": 2, "piid_on": 1},
    },
    {
        "model": "xiaomi.tv.mih1",
        "urn": "urn:miot-spec-v2:device:television:0000A010:xiaomi-mih1:2",
        "expect": {"siid": 2, "piid_on": 2, "invert_on": True},
    },
    {
        "model": "xiaomi.wifispeaker.lx06",
        "urn": "urn:miot-spec-v2:device:speaker:0000A015:xiaomi-lx06:2",
        "expect": {"piid_on": 0, "piid_volume": 1, "siid": 2},
    },
    {
        "model": "iot.switch.padw2p",
        "urn": "urn:miot-spec-v2:device:switch:0000A003:iot-padw2p:2:0000D00D",
        "expect": {
            "siid": 2,
            "piid_on": 1,
            "siid_power": 3,
            "piid_energy": 1,
            "piid_current": 2,
            "piid_power": 3,
            "piid_voltage": 4,
            "piid_temp": 9,
            "piid_fault": 5,
        },
    },
    {
        "model": "xiaomi.lock.s1",
        "urn": "urn:miot-spec-v2:device:lock:0000A038:xiaomi-s1:3",
        "expect": {"piid_on": 0, "piid_door": 1021, "siid": 3},
    },
]


def _urn(kind: str, name: str) -> str:
    return f"urn:miot-spec-v2:{kind}:{name}:00000001:x:1"


def _svc(iid: int, name: str, props: list | None = None, actions: list | None = None) -> dict:
    svc: dict = {"iid": iid, "type": _urn("service", name)}
    if props is not None:
        svc["properties"] = props
    if actions is not None:
        svc["actions"] = actions
    return svc


def _prop(iid: int, name: str) -> dict:
    return {"iid": iid, "type": _urn("property", name)}


def _act(iid: int, name: str) -> dict:
    return {"iid": iid, "type": _urn("action", name)}


def _extra_map(extras: list) -> dict:
    return {e["key"]: e for e in extras}


def check_harvest_extras() -> int:
    """Local extras harvest (no network). Mirrors miot_spec.c harvest_profile_extras."""
    failed = 0

    cam = slim_profile_from_instance(
        "mxiang.camera.c500ch",
        {
            "type": "urn:miot-spec-v2:device:camera:0000A01C:mxiang-c500ch:1",
            "services": [
                _svc(
                    2,
                    "camera-control",
                    [
                        _prop(1, "on"),
                        # Align with live C500 Spec iids (not historical fixture 10–13).
                        _prop(5, "time-watermark"),
                        _prop(6, "wdr-mode"),
                        _prop(8, "motion-tracking"),
                        _prop(9, "human-tracking"),
                    ],
                ),
                _svc(5, "motion-detection", [_prop(1, "motion-detection")]),
                _svc(4, "indicator-light", [_prop(1, "on")]),
            ],
        },
    )
    if not cam:
        print("FAIL camera: slim_profile_from_instance returned None")
        return 1
    if cam.get("profile_rev") != 11:
        failed += 1
        print(f"FAIL camera: profile_rev expected 11, got {cam.get('profile_rev')}")
    extras = cam.get("extras") or []
    if len(extras) > 12:
        failed += 1
        print(f"FAIL camera: extras n={len(extras)} exceeds 12")
    by = _extra_map(extras)
    cam_expect = {
        "indicator": {"kind": "switch", "siid": 4, "piid": 1, "aiid": 0},
        "motion_detection": {"kind": "switch", "siid": 5, "piid": 1, "aiid": 0},
        "human_tracking": {"kind": "switch", "siid": 2, "piid": 9, "aiid": 0},
        "time_watermark": {"kind": "switch", "siid": 2, "piid": 5, "aiid": 0},
        "wdr": {"kind": "switch", "siid": 2, "piid": 6, "aiid": 0},
    }
    if "motion_tracking" in by:
        failed += 1
        print("FAIL camera: motion_tracking extra should not be harvested")
    for key, fields in cam_expect.items():
        got = by.get(key)
        if not got:
            failed += 1
            print(f"FAIL camera: missing extra {key}")
            continue
        for fk, fv in fields.items():
            if got.get(fk) != fv:
                failed += 1
                print(f"FAIL camera: {key}.{fk} expected {fv}, got {got.get(fk)}")

    spk = slim_profile_from_instance(
        "xiaomi.wifispeaker.oh2p",
        {
            "type": "urn:miot-spec-v2:device:speaker:0000A015:xiaomi-oh2p:1",
            "services": [
                _svc(
                    2,
                    "speaker",
                    [_prop(1, "volume"), _prop(2, "no-disturb")],
                    [
                        _act(1, "play-text"),
                        _act(2, "execute-text-directive"),
                    ],
                ),
            ],
        },
    )
    if not spk:
        print("FAIL speaker: slim_profile_from_instance returned None")
        return 1
    if spk.get("piid_volume") != 1:
        failed += 1
        print(f"FAIL speaker: piid_volume expected 1, got {spk.get('piid_volume')}")
    extras = spk.get("extras") or []
    by = _extra_map(extras)
    if "volume" in by:
        failed += 1
        print("FAIL speaker: volume extra should be skipped when piid_volume is set")
    spk_expect = {
        "dnd": {"kind": "switch", "siid": 2, "piid": 2, "aiid": 0},
        "play_text": {"kind": "action", "siid": 2, "piid": 0, "aiid": 1},
        "execute_text": {"kind": "action", "siid": 2, "piid": 0, "aiid": 2},
    }
    for key, fields in spk_expect.items():
        got = by.get(key)
        if not got:
            failed += 1
            print(f"FAIL speaker: missing extra {key}")
            continue
        for fk, fv in fields.items():
            if got.get(fk) != fv:
                failed += 1
                print(f"FAIL speaker: {key}.{fk} expected {fv}, got {got.get(fk)}")

    plug = slim_profile_from_instance(
        "cuco.plug.v3",
        {
            "type": "urn:miot-spec-v2:device:outlet:0000A002:cuco-v3:2",
            "services": [
                _svc(2, "switch", [_prop(1, "on")]),
                _svc(3, "indicator-light", [_prop(1, "on")]),
            ],
        },
    )
    if not plug:
        print("FAIL plug: slim_profile_from_instance returned None")
        return 1
    by = _extra_map(plug.get("extras") or [])
    got = by.get("indicator")
    if not got or got.get("siid") != 3 or got.get("kind") != "switch":
        failed += 1
        print(f"FAIL plug: indicator extra expected siid=3 switch, got {got}")

    if failed:
        print(f"{failed} extras harvest case(s) failed")
        return 1
    print("PASS extras harvest (camera/speaker/plug)")
    return 0


def fetch_instance(urn: str) -> dict:
    enc = urllib.parse.quote(urn, safe="")
    url = f"https://miot-spec.org/miot-spec-v2/instance?type={enc}"
    with urllib.request.urlopen(url, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def check_spec_profiles() -> int:
    failed = 0
    for case in SPEC_PROFILE_CASES:
        model = case["model"]
        inst = fetch_instance(case["urn"])
        prof = slim_profile_from_instance(model, inst)
        if not prof:
            failed += 1
            print(f"FAIL {model}: slim_profile_from_instance returned None")
            continue
        for key, expected in case["expect"].items():
            got = prof.get(key, 0)
            if got != expected:
                failed += 1
                print(f"FAIL {model}: {key} expected {expected}, got {got}")
    if failed:
        print(f"{failed} Spec profile case(s) failed")
        return 1
    print(f"PASS Spec profiles ({len(SPEC_PROFILE_CASES)} cases)")
    return 0


def main() -> int:
    cases_path = Path(__file__).with_name("miot_profile_cases.json")
    cases = json.loads(cases_path.read_text(encoding="utf-8"))

    failed = 0
    for case in cases:
        model = case["model"]
        expected = MiotProfile(
            type=case["type"],
            siid=case["siid"],
            piid_on=case["piid_on"],
        )
        got = profile_for_model(model)
        if got.type != expected.type or got.siid != expected.siid or got.piid_on != expected.piid_on:
            failed += 1
            print(
                f"FAIL {model}: expected type={expected.type} siid={expected.siid} "
                f"piid_on={expected.piid_on}, got type={got.type} siid={got.siid} "
                f"piid_on={got.piid_on}"
            )

    if failed:
        print(f"{failed} case(s) failed")
        return 1

    print(f"PASS sync heuristics ({len(cases)} cases)")
    extras_rc = check_harvest_extras()
    if extras_rc != 0:
        return extras_rc
    return check_spec_profiles()


if __name__ == "__main__":
    sys.exit(main())

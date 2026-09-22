#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Minimal MiniHub Store server for LAN testing (default :8080).

Serves catalog.json and /files/<id>.mhub from the deploy directory:

  MINIHUB_PLUGINS_DEPLOY_DIR  (default ~/workspace/minihub_store)

Layout: catalog.json and <id>/<id>.mhub (zip only; MHMD .so stays in repo build/).
Sources remain under examples/plugins/ or MINIHUB_PLUGIN_DIRS.

Also assists MIoT Spec sync on Hub:
  POST /api/miot-spec/resolve   {"models":[…]} → {"urns":{model:{type,version,status}}}
  POST /api/miot-spec/profiles  {"models":[…]} → {"profiles":{model: slim_json}}
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_paths import module_source_dir, resolve_deploy_dir  # noqa: E402

try:
    from bridges import huawei_mesh as bridge_huawei
except ImportError:  # pragma: no cover
    bridge_huawei = None  # type: ignore
try:
    from bridges import midea_meiju as bridge_midea
except ImportError:  # pragma: no cover
    bridge_midea = None  # type: ignore


def load_catalog(catalog: Path) -> list:
    if catalog.exists():
        return json.loads(catalog.read_text(encoding="utf-8"))
    return []


def resolve_store_file(deploy: Path, name: str) -> Path | None:
    """Map /files/<id>.mhub → deploy/<id>/<id>.mhub; .json falls back to source module.json."""
    if not name or "/" in name or name.startswith("."):
        return None
    path = Path(name)
    stem, suffix = path.stem, path.suffix.lower()
    mod_dir = deploy / stem
    if suffix == ".json":
        src = module_source_dir(stem) / "module.json"
        return src if src.is_file() else None
    if not mod_dir.is_dir():
        return None
    if suffix == ".mhub":
        candidate = mod_dir / f"{stem}.mhub"
        return candidate if candidate.is_file() else None
    return None


def load_instances_index(cache: Path) -> dict[str, dict]:
    """model -> {type, version, status} from miot-spec.org instances (cached on disk)."""
    if cache.exists() and cache.stat().st_size > 1000:
        data = json.loads(cache.read_text(encoding="utf-8"))
    else:
        url = "https://miot-spec.org/miot-spec-v2/instances?status=all"
        print(f"[store] downloading Spec instances index → {cache}")
        with urllib.request.urlopen(url, timeout=180) as resp:
            raw = resp.read()
        cache.write_bytes(raw)
        data = json.loads(raw.decode("utf-8"))
    out: dict[str, dict] = {}
    for v in data.get("instances") or []:
        if not isinstance(v, dict):
            continue
        m = v.get("model")
        if not m:
            continue
        prev = out.get(m)
        if prev and prev.get("status") == "released" and v.get("status") != "released":
            continue
        if prev and int(v.get("version") or 0) < int(prev.get("version") or 0):
            continue
        out[m] = {
            "type": v.get("type"),
            "version": v.get("version"),
            "status": v.get("status"),
        }
    return out


def _device_from_urn(urn: str) -> str:
    marker = ":device:"
    i = urn.find(marker)
    if i < 0:
        return ""
    rest = urn[i + len(marker) :]
    return rest.split(":", 1)[0]


def _prop_name(type_urn: str) -> str:
    marker = ":property:"
    i = type_urn.find(marker)
    if i < 0:
        return ""
    rest = type_urn[i + len(marker) :]
    return rest.split(":", 1)[0]


def _act_name(type_urn: str) -> str:
    marker = ":action:"
    i = type_urn.find(marker)
    if i < 0:
        return ""
    rest = type_urn[i + len(marker) :]
    return rest.split(":", 1)[0]


def _svc_name(type_urn: str) -> str:
    marker = ":service:"
    i = type_urn.find(marker)
    if i < 0:
        return ""
    rest = type_urn[i + len(marker) :]
    return rest.split(":", 1)[0]


def _on_service_rank(device: str, svc: str) -> int:
    if not svc:
        return 0
    if svc in ("indicator-light", "doorbell", "alarm", "air-conditioner-alarm"):
        return -1
    if device == "camera" and svc in ("camera-control", "camera"):
        return 100
    if device == "television" and svc == "television":
        return 100
    if device == "lock" and svc in ("lock", "door-lock"):
        return 100
    if device in ("light", "outlet", "switch") and svc in ("light", "switch", "outlet"):
        return 100
    if svc in ("screen", "indicator-light"):
        return 5
    if svc == "door-lock-camera":
        return 5
    return 10


def _str_case_contains(hay: str, needle: str) -> bool:
    return needle.lower() in hay.lower()


def _parse_motor_value_list(prop: dict, prof: dict) -> None:
    vl = prop.get("value-list")
    if not isinstance(vl, list):
        return
    for ent in vl:
        if not isinstance(ent, dict):
            continue
        val = ent.get("value")
        desc = ent.get("description")
        if not isinstance(val, int) or not isinstance(desc, str) or val < 0 or val > 255:
            continue
        if _str_case_contains(desc, "Open"):
            prof["motor_open"] = val
        elif _str_case_contains(desc, "Close"):
            prof["motor_close"] = val
        elif _str_case_contains(desc, "Pause"):
            prof["motor_pause"] = val


def _is_cover_device(device: str) -> bool:
    return device in ("curtain", "airer", "window-opener")


MIOT_SPEC_EXTRAS_MAX = 12
# Mirror miot_spec.c harvest_profile_extras maps: key, kind, is_action, hints.
_EXTRA_MAPS: list[tuple[str, str, bool, tuple[str, ...]]] = [
    ("indicator", "switch", False, ("indicator-light", "indicator")),
    ("motion_detection", "switch", False, ("motion-detection",)),
    ("human_tracking", "switch", False, ("human-tracking",)),
    ("time_watermark", "switch", False, ("time-watermark",)),
    ("wdr", "switch", False, ("wide-dynamic-range", "wdr-mode")),
    ("volume", "number", False, ("volume",)),
    ("dnd", "switch", False, ("no-disturb", "do-not-disturb")),
    ("play_text", "action", True, ("play-text", "play_text")),
    ("execute_text", "action", True, ("execute-text-directive", "text-directive")),
]


def _extra_hint_score(name: str, hints: tuple[str, ...]) -> int:
    """Substring, case-insensitive; exact URN name scores higher than a partial hit."""
    if not name:
        return 0
    nlen = 0
    while nlen < len(name) and name[nlen] != ":":
        nlen += 1
    best = 0
    for hint in hints:
        if not hint or not _str_case_contains(name, hint):
            continue
        sc = 2 if nlen == len(hint) else 1
        if sc > best:
            best = sc
    return best


def harvest_profile_extras(inst: dict, prof: dict) -> None:
    """Match miot_spec.c harvest_profile_extras (no heap check on Store)."""
    services = inst.get("services") or []
    if not isinstance(services, list):
        return
    extras: list[dict] = []
    for key, kind, is_action, hints in _EXTRA_MAPS:
        if len(extras) >= MIOT_SPEC_EXTRAS_MAX:
            break
        if key == "volume" and int(prof.get("piid_volume") or 0) > 0:
            continue
        best = 0
        best_siid = best_piid = best_aiid = 0
        for svc in services:
            if not isinstance(svc, dict):
                continue
            siid = svc.get("iid")
            if not isinstance(siid, int):
                continue
            st = svc.get("type")
            svc_name = _svc_name(st) if isinstance(st, str) else ""
            if is_action:
                actions = svc.get("actions")
                if not isinstance(actions, list):
                    continue
                for act in actions:
                    if not isinstance(act, dict):
                        continue
                    aiid = act.get("iid")
                    at = act.get("type")
                    if not isinstance(aiid, int) or not isinstance(at, str):
                        continue
                    name = _act_name(at)
                    sc = _extra_hint_score(name, hints)
                    if sc > best:
                        best = sc
                        best_siid = siid
                        best_piid = 0
                        best_aiid = aiid
            else:
                props = svc.get("properties")
                if not isinstance(props, list):
                    continue
                svc_sc = _extra_hint_score(svc_name, hints)
                for prop in props:
                    if not isinstance(prop, dict):
                        continue
                    piid = prop.get("iid")
                    pt = prop.get("type")
                    if not isinstance(piid, int) or not isinstance(pt, str):
                        continue
                    name = _prop_name(pt)
                    sc = _extra_hint_score(name, hints)
                    if name == "on" and svc_sc > sc:
                        sc = svc_sc
                    if sc > best:
                        best = sc
                        best_siid = siid
                        best_piid = piid
                        best_aiid = 0
        if best > 0:
            extras.append(
                {
                    "key": key,
                    "kind": kind,
                    "siid": best_siid,
                    "piid": best_piid,
                    "aiid": best_aiid,
                }
            )
    if extras:
        prof["extras"] = extras


def slim_profile_from_instance(model: str, inst: dict) -> dict | None:
    """Match miot_spec.c fill_profile_from_instance + save_slim_profile shape."""
    urn = inst.get("type") or ""
    if not isinstance(urn, str) or not urn:
        return None
    device = _device_from_urn(urn)
    if not device:
        return None
    prof = {
        "model": model,
        "urn": urn,
        "device": device,
        "siid": 2,
        "piid_on": 0,
        "piid_bright": 0,
        "piid_ct": 0,
        "piid_color": 0,
        "supports_brightness": False,
        "supports_color_temp": False,
        "supports_color": False,
    }
    best_on_rank = -1
    sleep_siid = sleep_piid = 0
    volume_siid = volume_piid = 0
    door_siid = door_piid = 0
    for svc in inst.get("services") or []:
        if not isinstance(svc, dict):
            continue
        siid = svc.get("iid")
        props = svc.get("properties")
        if not isinstance(siid, int) or not isinstance(props, list):
            continue
        st = svc.get("type")
        svc_name = _svc_name(st) if isinstance(st, str) else ""
        for prop in props:
            if not isinstance(prop, dict):
                continue
            piid = prop.get("iid")
            pt = prop.get("type")
            if not isinstance(piid, int) or not isinstance(pt, str):
                continue
            name = _prop_name(pt)
            is_cover = _is_cover_device(device)
            if name == "on" and not is_cover and device != "lock" and device != "magnet-sensor":
                rank = _on_service_rank(device, svc_name)
                if rank > best_on_rank:
                    best_on_rank = rank
                    prof["siid"] = siid
                    prof["piid_on"] = piid
                    prof.pop("invert_on", None)
            elif name == "contact-state":
                # Door/window magnets: primary Hub value (cloud prop/get).
                prof["siid"] = siid
                prof["piid_on"] = piid
                prof.pop("invert_on", None)
                best_on_rank = 200
            elif name == "illumination":
                prof["piid_illum"] = piid
            elif name == "no-motion-duration":
                prof["piid_no_motion"] = piid
            elif name == "battery-level":
                prof["siid_battery"] = siid
                prof["piid_battery"] = piid
            elif name == "video-codec":
                prof["siid_stream"] = siid
                prof["piid_video_codec"] = piid
            elif name == "audio-codec":
                if int(prof.get("siid_stream") or 0) <= 0:
                    prof["siid_stream"] = siid
                prof["piid_audio_codec"] = piid
            elif name == "sleep-mode":
                if sleep_piid <= 0 or svc_name == "television":
                    sleep_siid, sleep_piid = siid, piid
            elif name == "volume":
                if volume_piid <= 0 or svc_name == "speaker":
                    volume_siid, volume_piid = siid, piid
            elif name == "door-state":
                if door_piid <= 0:
                    door_siid, door_piid = siid, piid
            elif name == "brightness":
                if int(prof.get("piid_on") or 0) <= 0:
                    prof["siid"] = siid
                prof["piid_bright"] = piid
                prof["supports_brightness"] = True
                vr = prop.get("value-range")
                if isinstance(vr, list) and len(vr) >= 2 and isinstance(vr[1], (int, float)) and vr[1] >= 1:
                    prof["bright_max"] = int(vr[1])
            elif name == "color-temperature":
                if int(prof.get("piid_on") or 0) <= 0:
                    prof["siid"] = siid
                prof["piid_ct"] = piid
                prof["supports_color_temp"] = True
            elif name == "color":
                if int(prof.get("piid_on") or 0) <= 0:
                    prof["siid"] = siid
                prof["piid_color"] = piid
                prof["supports_color"] = True
            elif name == "mode":
                if int(prof.get("piid_on") or 0) <= 0 and int(prof.get("piid_position") or 0) <= 0:
                    prof["siid"] = siid
                prof["piid_mode"] = piid
            elif name == "target-temperature":
                if int(prof.get("piid_on") or 0) <= 0:
                    prof["siid"] = siid
                prof["piid_target_temp"] = piid
            elif name == "fan-level":
                prof["siid_fan"] = siid
                prof["piid_fan"] = piid
            elif name == "temperature":
                prof["siid_env"] = siid
                prof["piid_temp"] = piid
            elif name == "power-consumption":
                prof["siid_power"] = siid
                prof["piid_energy"] = piid
            elif name == "electric-current":
                prof["siid_power"] = siid
                prof["piid_current"] = piid
            elif name == "electric-power":
                prof["siid_power"] = siid
                prof["piid_power"] = piid
            elif name == "voltage":
                prof["siid_power"] = siid
                prof["piid_voltage"] = piid
            elif name == "fault":
                if int(prof.get("piid_fault") or 0) <= 0:
                    prof["piid_fault"] = piid
            elif name == "current-position":
                prof["siid"] = siid
                prof["piid_position"] = piid
            elif name == "target-position":
                prof["siid"] = siid
                prof["piid_target_position"] = piid
            elif name == "motor-control":
                prof["siid_motor"] = siid
                prof["piid_motor"] = piid
                _parse_motor_value_list(prop, prof)
    if _is_cover_device(device):
        prof["piid_on"] = 0
        prof.pop("invert_on", None)
    elif prof.get("piid_on", 0) <= 0:
        if sleep_piid > 0 and device in ("television", "projector", "tv-box"):
            prof["siid"] = sleep_siid
            prof["piid_on"] = sleep_piid
            prof["invert_on"] = True
        elif device in ("speaker", "intelligent-speaker"):
            if volume_piid > 0:
                prof["siid"] = volume_siid
                prof["piid_volume"] = volume_piid
            prof["piid_on"] = 0
        elif device == "lock":
            if door_piid > 0:
                prof["siid"] = door_siid
                prof["piid_door"] = door_piid
            prof["piid_on"] = 0
        elif device in ("motion-sensor", "remote-control"):
            prof["piid_on"] = 0
    if volume_piid > 0 and not prof.get("piid_volume") and device in (
        "speaker",
        "intelligent-speaker",
    ):
        prof["piid_volume"] = volume_piid
    if door_piid > 0 and not prof.get("piid_door") and device == "lock":
        prof["piid_door"] = door_piid
    harvest_profile_extras(inst, prof)
    prof["profile_rev"] = 11
    slim = {
        k: v
        for k, v in prof.items()
        if k
        in (
            "model",
            "urn",
            "device",
            "siid",
            "piid_on",
            "piid_bright",
            "bright_max",
            "piid_ct",
            "piid_color",
            "supports_brightness",
            "supports_color_temp",
            "supports_color",
            "piid_illum",
            "siid_battery",
            "piid_battery",
            "siid_stream",
            "piid_video_codec",
            "piid_audio_codec",
            "invert_on",
            "profile_rev",
        )
        or v
    }
    return slim


def fetch_instance(urn: str) -> dict | None:
    enc = urllib.parse.quote(urn, safe="")
    url = f"https://miot-spec.org/miot-spec-v2/instance?type={enc}"
    try:
        with urllib.request.urlopen(url, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as exc:  # noqa: BLE001 — surface in store log
        print(f"[store] instance fetch failed for {urn}: {exc}")
        return None


def build_slim_for_model(
    model: str, urn: str, slim_path: Path, *, force: bool = False
) -> dict | None:
    if not force and slim_path.is_file() and slim_path.stat().st_size > 20:
        try:
            return json.loads(slim_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    if not urn:
        return None
    inst = fetch_instance(urn)
    if not inst:
        return None
    slim = slim_profile_from_instance(model, inst)
    if not slim:
        return None
    slim_path.parent.mkdir(parents=True, exist_ok=True)
    slim_path.write_text(json.dumps(slim, ensure_ascii=False), encoding="utf-8")
    return slim


LIVING_ROOM_SLIM_MODELS = (
    "cuco.plug.v3",
    "yeelink.light.mbulb3",
    "mxiang.camera.c500ch",
    "xiaomi.wifispeaker.oh2p",
    "isa.magnet.dw2hl",
    "lumi.remote.mcn001",
)


def rebuild_slim_cache(deploy: Path, models: list[str] | None = None) -> int:
    """Force-rebuild Store slim JSON from Spec instances (rev 4 extras)."""
    slim_dir = deploy / "miot_spec_slim"
    slim_dir.mkdir(parents=True, exist_ok=True)
    idx = load_instances_index(deploy / "miot_spec_instances.json")
    wanted: list[str] = []
    if models:
        wanted.extend(models)
    else:
        wanted.extend(LIVING_ROOM_SLIM_MODELS)
        for path in sorted(slim_dir.glob("*.json")):
            wanted.append(path.stem)
    seen: set[str] = set()
    ordered: list[str] = []
    for m in wanted:
        if m and m not in seen:
            seen.add(m)
            ordered.append(m)
    ok = 0
    failed = 0
    for model in ordered:
        ent = idx.get(model) or {}
        urn = ent.get("type") or ""
        slim_path = slim_dir / f"{model.replace('/', '_')}.json"
        if not urn and slim_path.is_file():
            try:
                urn = json.loads(slim_path.read_text(encoding="utf-8")).get("urn") or ""
            except json.JSONDecodeError:
                urn = ""
        slim = build_slim_for_model(model, urn, slim_path, force=True)
        if slim:
            extras_n = len(slim.get("extras") or [])
            print(f"[store] rebuilt {model} rev={slim.get('profile_rev')} extras={extras_n}")
            ok += 1
        else:
            print(f"[store] rebuild failed {model} urn={urn or '(missing)'}")
            failed += 1
    print(f"[store] slim rebuild ok={ok} failed={failed}")
    return 0 if failed == 0 else 1


def make_handler(deploy: Path, catalog: Path):
    instances_cache = deploy / "miot_spec_instances.json"
    slim_dir = deploy / "miot_spec_slim"
    slim_dir.mkdir(parents=True, exist_ok=True)
    instances_index: dict[str, dict] | None = None

    def get_index() -> dict[str, dict]:
        nonlocal instances_index
        if instances_index is None:
            instances_index = load_instances_index(instances_cache)
            print(f"[store] Spec index models={len(instances_index)}")
        return instances_index

    def slim_path(model: str) -> Path:
        safe = model.replace("/", "_")
        return slim_dir / f"{safe}.json"

    def get_or_build_slim(model: str) -> dict | None:
        path = slim_path(model)
        ent = get_index().get(model) or {}
        return build_slim_for_model(model, ent.get("type") or "", path)

    def get_or_build_slim_many(models: list) -> dict[str, dict]:
        """Resolve many models; fetch missing instances in parallel (LAN sync budget)."""
        idx = get_index()
        profiles: dict[str, dict] = {}
        pending: list[tuple[str, str, Path]] = []
        for m in models:
            if not isinstance(m, str) or not m:
                continue
            path = slim_path(m)
            if path.is_file() and path.stat().st_size > 20:
                try:
                    profiles[m] = json.loads(path.read_text(encoding="utf-8"))
                    continue
                except json.JSONDecodeError:
                    pass
            urn = (idx.get(m) or {}).get("type") or ""
            if urn:
                pending.append((m, urn, path))
        if pending:
            workers = min(12, len(pending))
            with ThreadPoolExecutor(max_workers=workers) as pool:
                futs = {
                    pool.submit(build_slim_for_model, m, urn, path): m for m, urn, path in pending
                }
                for fut in as_completed(futs):
                    m = futs[fut]
                    try:
                        slim = fut.result()
                    except Exception as exc:  # noqa: BLE001
                        print(f"[store] slim build error {m}: {exc}")
                        continue
                    if slim:
                        profiles[m] = slim
        return profiles

    class Handler(BaseHTTPRequestHandler):
        def _cors(self) -> None:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")

        def _json(self, code: int, obj) -> None:
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self._cors()
            self.end_headers()
            self.wfile.write(body)

        def do_OPTIONS(self) -> None:
            self.send_response(204)
            self._cors()
            self.end_headers()

        def do_GET(self) -> None:
            if self.path == "/api/catalog":
                self._json(200, load_catalog(catalog))
                return
            if self.path.startswith("/files/"):
                name = self.path.split("/files/", 1)[1]
                path = resolve_store_file(deploy, name)
                if not path:
                    self.send_error(404)
                    return
                data = path.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self._cors()
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_error(404)

        def do_POST(self) -> None:
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length > 0 else b"{}"
            try:
                req = json.loads(raw.decode("utf-8"))
            except json.JSONDecodeError:
                self._json(400, {"error": "invalid json"})
                return
            if not isinstance(req, dict):
                self._json(400, {"error": "json object required"})
                return

            # --- Store protocol bridges (Huawei / Midea) ---
            if self.path.startswith("/api/bridge/"):
                if self.path == "/api/bridge/huawei/login":
                    if bridge_huawei is None:
                        self._json(500, {"ok": False, "error": "huawei bridge import failed"})
                        return
                    out = bridge_huawei.login(
                        req.get("host") or "",
                        req.get("username") or req.get("user") or "admin",
                        req.get("password") or "",
                        int(req.get("port") or 80),
                    )
                    self._json(200, out)
                    return
                if self.path == "/api/bridge/huawei/snapshot":
                    if bridge_huawei is None:
                        self._json(500, {"ok": False, "error": "huawei bridge import failed"})
                        return
                    out = bridge_huawei.snapshot(
                        req.get("host") or "",
                        req.get("username") or req.get("user") or "admin",
                        req.get("password") or "",
                        int(req.get("port") or 80),
                    )
                    self._json(200, out)
                    return
                if self.path == "/api/bridge/midea/login":
                    if bridge_midea is None:
                        self._json(500, {"ok": False, "error": "midea bridge import failed"})
                        return
                    out = bridge_midea.login(
                        req.get("account") or req.get("username") or "",
                        req.get("password") or "",
                        req.get("server") if req.get("server") is not None else 2,
                    )
                    self._json(200, out)
                    return
                if self.path == "/api/bridge/midea/devices":
                    if bridge_midea is None:
                        self._json(500, {"ok": False, "error": "midea bridge import failed"})
                        return
                    account = req.get("account") or req.get("username") or ""
                    password = req.get("password") or ""
                    server = req.get("server") if req.get("server") is not None else 2
                    session = req.get("session") if isinstance(req.get("session"), dict) else None
                    if session is None and req.get("access_token"):
                        session = {
                            "account": account,
                            "access_token": req.get("access_token"),
                            "aes_key": req.get("aes_key"),
                            "login_id": req.get("login_id"),
                            "device_id": req.get("device_id"),
                            "server": server,
                        }
                    if session and session.get("access_token"):
                        if not session.get("account") and account:
                            session["account"] = account
                        if session.get("server") is None:
                            session["server"] = server
                        out = bridge_midea.list_devices(session, password=password)
                        print(
                            f"[store] midea devices ok={out.get('ok')} "
                            f"n={len(out.get('devices') or [])} err={out.get('error')}"
                        )
                    elif account and password:
                        lr = bridge_midea.login(account, password, server)
                        if not lr.get("ok"):
                            self._json(200, {"ok": False, "error": lr.get("error"), "devices": []})
                            return
                        sess = lr.get("session") or lr
                        out = bridge_midea.list_devices(sess, password=password)
                        print(
                            f"[store] midea devices(login) ok={out.get('ok')} "
                            f"n={len(out.get('devices') or [])} err={out.get('error')}"
                        )
                    else:
                        out = {"ok": False, "error": "account/password or session required", "devices": []}
                    self._json(200, out)
                    return
                if self.path in ("/api/bridge/midea/status", "/api/bridge/midea/control"):
                    if bridge_midea is None:
                        self._json(500, {"ok": False, "error": "midea bridge import failed"})
                        return
                    account = req.get("account") or req.get("username") or ""
                    password = req.get("password") or ""
                    server = req.get("server") if req.get("server") is not None else 2
                    appliance_id = req.get("appliance_id") or req.get("id") or req.get("applianceCode")
                    session = req.get("session") if isinstance(req.get("session"), dict) else None
                    if session is None and req.get("access_token"):
                        session = {
                            "account": account,
                            "access_token": req.get("access_token"),
                            "aes_key": req.get("aes_key"),
                            "login_id": req.get("login_id"),
                            "device_id": req.get("device_id"),
                            "server": server,
                        }
                    if not appliance_id:
                        self._json(200, {"ok": False, "error": "appliance_id required"})
                        return
                    if not session or not session.get("access_token"):
                        if not (account and password):
                            self._json(200, {"ok": False, "error": "account/password or session required"})
                            return
                        lr = bridge_midea.login(account, password, server)
                        if not lr.get("ok"):
                            self._json(200, {"ok": False, "error": lr.get("error")})
                            return
                        session = lr.get("session") or lr
                    else:
                        if not session.get("account") and account:
                            session["account"] = account
                        if session.get("server") is None:
                            session["server"] = server
                    if self.path.endswith("/status"):
                        out = bridge_midea.get_status(
                            session,
                            appliance_id,
                            password=password,
                            query=req.get("query") if isinstance(req.get("query"), dict) else None,
                        )
                    else:
                        # Prefer explicit control payload (temperature etc.).
                        control = req.get("control") if isinstance(req.get("control"), dict) else None
                        if control:
                            out = bridge_midea.set_fields(
                                session, appliance_id, control, password=password
                            )
                        else:
                            on = req.get("on")
                            if on is None:
                                power = req.get("power")
                                if isinstance(power, str):
                                    on = power.strip().lower() in ("on", "1", "true")
                                elif isinstance(power, bool):
                                    on = power
                                else:
                                    on = True
                            out = bridge_midea.set_power(
                                session, appliance_id, bool(on), password=password
                            )
                    print(
                        f"[store] midea {self.path.rsplit('/', 1)[-1]} "
                        f"id={appliance_id} ok={out.get('ok')} err={out.get('error')}"
                    )
                    self._json(200, out)
                    return
                self._json(404, {"ok": False, "error": f"unknown bridge path {self.path}"})
                return

            models = req.get("models") or []
            if not isinstance(models, list):
                self._json(400, {"error": "models must be array"})
                return

            if self.path == "/api/miot-spec/resolve":
                idx = get_index()
                result = {}
                for m in models:
                    if not isinstance(m, str) or not m:
                        continue
                    ent = idx.get(m)
                    if ent and ent.get("type"):
                        result[m] = ent
                self._json(200, {"urns": result})
                return

            if self.path == "/api/miot-spec/profiles":
                profiles = get_or_build_slim_many(models)
                print(f"[store] Spec profiles → {len(profiles)}/{len(models)}")
                self._json(200, {"profiles": profiles})
                return

            self.send_error(404)

        def log_message(self, fmt: str, *args) -> None:
            print(f"[store] {self.address_string()} {fmt % args}")

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MINIHUB_STORE_BIND", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("MINIHUB_STORE_PORT", "8080")))
    parser.add_argument(
        "--deploy",
        type=Path,
        default=None,
        help="deploy root (default: MINIHUB_PLUGINS_DEPLOY_DIR or ~/workspace/minihub_store)",
    )
    parser.add_argument(
        "--rebuild-slim",
        action="store_true",
        help="force-rebuild miot_spec_slim/*.json from Spec instances, then exit",
    )
    args = parser.parse_args()
    deploy = resolve_deploy_dir(args.deploy)
    if args.rebuild_slim:
        return rebuild_slim_cache(deploy)
    catalog = deploy / "catalog.json"
    Handler = make_handler(deploy, catalog)
    srv = HTTPServer((args.host, args.port), Handler)
    print(f"MiniHub Store listening on http://{args.host}:{args.port}")
    print(f"  deploy:  {deploy}")
    print(f"  catalog: {catalog if catalog.exists() else '(missing — run sync_store_catalog.py)'}")
    print(f"  modules: {deploy}/<id>/<id>.mhub")
    print("  Spec:    POST /api/miot-spec/resolve|profiles")
    print("  Bridge:  POST /api/bridge/huawei/login|snapshot")
    print("  Bridge:  POST /api/bridge/midea/login|devices|status|control")
    srv.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

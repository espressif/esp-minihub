#!/usr/bin/env python3
"""Build the exhaustive four-way (BLE Mesh / Matter / Zigbee / MiniHub) table.

Spec data is pulled from the OFFICIAL connectedhomeip auto-generated pages
(2026-11-12):
  * Matter device types :
    project-chip.github.io/connectedhomeip-doc/ids_and_codes/spec_device_types.html
    (105 device types, each with hex ID + name)  -> MATTER_DEVICES
  * Zigbee/Matter ZCL clusters :
    project-chip.github.io/connectedhomeip-doc/ids_and_codes/spec_clusters.html
    (fetched snapshot: 156 clusters, each with hex ID + name)  -> ZCL_CLUSTERS
  * Zigbee ZHA device IDs : Zigbee HA Public Profile (NXP) + Zigbee Lighting &
    Occupancy Device Spec (CSA) snippets.
  * BLE Mesh models      : SIG Model ID tables (cfanz / Silabs / Wikiwand).

Every spec row is tagged with a MiniHub-support flag so the gap is explicit:
  device types : "yes(minihub_type)" / "param" (param exists, no classify yet)
                 / "no"
  clusters     : matter=yes/no, zigbee=yes/no  (BLE uses models, not clusters)

MiniHub side is parsed live from the three capability maps, so re-running after
adding types refreshes the grounded support columns.

Outputs (tools/spec_correspondence/):
  spec_matter_device_types.csv  spec_zcl_clusters.csv
  spec_zigbee_zha_device_ids.csv  spec_ble_mesh_models.csv
  correspondence_devices.csv  correspondence_params.csv  correspondence.md
  README.md
"""
import json, csv, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

MAPS = {
    "matter":   os.path.join(ROOT, "components/minihub_protocol_adapters/adapter_matter/matter_map.json"),
    "zigbee":   os.path.join(ROOT, "components/minihub_zigbee/config/zigbee_cluster_map.json"),
    "ble_mesh": os.path.join(ROOT, "components/minihub_ble_mesh/config/ble_mesh_map.json"),
}

# ---------------------------------------------------------------------------
# 1) FULL Matter Device Library (105 entries, official spec, 2026-11-12)
# ---------------------------------------------------------------------------
MATTER_DEVICES = [
    (0x000A, "Door Lock"), (0x000B, "Door Lock Controller"), (0x000E, "Aggregator"),
    (0x000F, "Generic Switch"), (0x0011, "Power Source"), (0x0012, "OTA Requestor"),
    (0x0013, "Bridged Node"), (0x0014, "OTA Provider"), (0x0015, "Contact Sensor"),
    (0x0016, "Root Node"), (0x0017, "Solar Power"), (0x0018, "Battery Storage"),
    (0x0019, "Secondary Network Interface"), (0x0020, "Streaming Audio Player"),
    (0x0021, "Casting Audio Player"), (0x0022, "Speaker"), (0x0023, "Casting Video Player"),
    (0x0024, "Content App"), (0x0027, "Mode Select"), (0x0028, "Basic Video Player"),
    (0x0029, "Casting Video Client"), (0x002A, "Video Remote Control"), (0x002B, "Fan"),
    (0x002C, "Air Quality Sensor"), (0x002D, "Air Purifier"), (0x0040, "Irrigation System"),
    (0x0041, "Water Freeze Detector"), (0x0042, "Water Valve"), (0x0043, "Water Leak Detector"),
    (0x0044, "Rain Sensor"), (0x0045, "Soil Sensor"), (0x0070, "Refrigerator"),
    (0x0071, "Temperature Controlled Cabinet"), (0x0072, "Room Air Conditioner"),
    (0x0073, "Laundry Washer"), (0x0074, "Robotic Vacuum Cleaner"), (0x0075, "Dishwasher"),
    (0x0076, "Smoke CO Alarm"), (0x0077, "Cook Surface"), (0x0078, "Cooktop"),
    (0x0079, "Microwave Oven"), (0x007A, "Extractor Hood"), (0x007B, "Oven"),
    (0x007C, "Laundry Dryer"), (0x007D, "Humidity Conditioner"),
    (0x0090, "Network Infrastructure Manager"), (0x0091, "Thread Border Router"),
    (0x0092, "Commissioning By Proxy"), (0x0100, "On/Off Light"), (0x0101, "Dimmable Light"),
    (0x0103, "On/Off Light Switch"), (0x0104, "Dimmer Switch"), (0x0105, "Color Dimmer Switch"),
    (0x0106, "Light Sensor"), (0x0107, "Occupancy Sensor"), (0x010A, "On/Off Plug-in Unit"),
    (0x010B, "Dimmable Plug-In Unit"), (0x010C, "Color Temperature Light"),
    (0x010D, "Extended Color Light"), (0x010F, "Mounted On/Off Control"),
    (0x0110, "Mounted Dimmable Load Control"), (0x0130, "Joint Fabric Administrator"),
    (0x0140, "Intercom"), (0x0141, "Audio Doorbell"), (0x0142, "Camera"),
    (0x0143, "Video Doorbell"), (0x0144, "Floodlight Camera"), (0x0145, "Snapshot Camera"),
    (0x0146, "Chime"), (0x0147, "Camera Controller"), (0x0148, "Doorbell"),
    (0x0149, "AV Analysis Node"), (0x0150, "Ambient Context Sensor"),
    (0x0152, "Proximity Ranger"), (0x0202, "Window Covering"),
    (0x0203, "Window Covering Controller"), (0x0230, "Closure"), (0x0231, "Closure Panel"),
    (0x023E, "Closure Controller"), (0x0300, "Heating/Cooling Unit"), (0x0301, "Thermostat"),
    (0x0302, "Temperature Sensor"), (0x0303, "Pump"), (0x0304, "Pump Controller"),
    (0x0305, "Pressure Sensor"), (0x0306, "Flow Sensor"), (0x0307, "Humidity Sensor"),
    (0x0309, "Heat Pump"), (0x030A, "Thermostat Controller"), (0x050C, "Energy EVSE"),
    (0x050D, "Device Energy Management"), (0x050F, "Water Heater"),
    (0x0510, "Electrical Sensor"), (0x0511, "Electrical Utility Meter"),
    (0x0512, "Meter Reference Point"), (0x0513, "Electrical Energy Tariff"),
    (0x0514, "Electrical Meter"), (0x0515, "Arc Fault Circuit Interrupter"),
    (0x0516, "Electrical Circuit Breaker"), (0x0517, "Electrical Distribution Enclosure"),
    (0x0518, "Electrical Surge Protector"), (0x0519, "Residual Current Circuit Breaker"),
    (0x0520, "Auxiliary Load Switch"), (0x0840, "Control Bridge"), (0x0850, "On/Off Sensor"),
]

# Matter device name -> MiniHub classify type (FULL device-type support)
MATTER_DEVICE_SUPPORT = {
    "On/Off Light": "light", "Dimmable Light": "light", "Color Temperature Light": "light",
    "Extended Color Light": "light", "On/Off Plug-in Unit": "light",
    "Dimmable Plug-In Unit": "light", "Mounted On/Off Control": "light",
    "Mounted Dimmable Load Control": "light",
    "On/Off Light Switch": "switch", "Dimmer Switch": "switch",
    "Color Dimmer Switch": "switch", "Generic Switch": "switch", "Control Bridge": "switch",
    "Temperature Sensor": "sensor_temp_humidity", "Humidity Sensor": "sensor_temp_humidity",
    "Light Sensor": "sensor_illuminance", "Occupancy Sensor": "sensor_occupancy",
    "Air Quality Sensor": "sensor_air_quality",
}
# Matter device name -> existing MiniHub param (param exists but no classify yet)
MATTER_DEVICE_PARAM = {
    "Contact Sensor": "contact", "Pressure Sensor": "pressure_hpa",
    "Flow Sensor": "flow", "Smoke CO Alarm": "smoke", "Water Leak Detector": "leak",
    "Water Freeze Detector": "freeze", "Rain Sensor": "rain", "Soil Sensor": "soil",
    "Door Lock": "lock_state", "Window Covering": "covering_position",
    "Thermostat": "thermostat_setpoint",
}

# ---------------------------------------------------------------------------
# 2) FULL Zigbee/Matter ZCL clusters (159 entries, official spec, 2026-11-12)
# ---------------------------------------------------------------------------
ZCL_CLUSTERS = [
    (0x0003, "Identify"), (0x0004, "Groups"), (0x0005, "Scenes"), (0x0006, "On/Off"),
    (0x0008, "Level Control"), (0x001C, "Pulse Width Modulation"), (0x001D, "Descriptor"),
    (0x001E, "Binding"), (0x001F, "Access Control"), (0x0025, "Actions"),
    (0x0028, "Basic Information"), (0x0029, "OTA Software Update Provider"),
    (0x002A, "OTA Software Update Requestor"), (0x002B, "Localization Configuration"),
    (0x002C, "Time Format Localization"), (0x002D, "Unit Localization"),
    (0x002E, "Power Source Configuration"), (0x002F, "Power Source"),
    (0x0030, "General Commissioning"), (0x0031, "Network Commissioning"),
    (0x0032, "Diagnostic Logs"), (0x0033, "General Diagnostics"),
    (0x0034, "Software Diagnostics"), (0x0035, "Thread Network Diagnostics"),
    (0x0036, "Wi-Fi Network Diagnostics"), (0x0037, "Ethernet Network Diagnostics"),
    (0x0038, "Time Synchronization"), (0x0039, "Bridged Device Basic Information"),
    (0x003B, "Switch"), (0x003C, "Administrator Commissioning"),
    (0x003E, "Operational Credentials"), (0x003F, "Group Key Management"),
    (0x0040, "Fixed Label"), (0x0041, "User Label"), (0x0042, "Proxy Configuration"),
    (0x0043, "Proxy Discovery"), (0x0044, "Valid Proxies"), (0x0045, "Boolean State"),
    (0x0046, "ICD Management"), (0x0048, "Oven Cavity Operational State"),
    (0x0049, "Oven Mode"), (0x004A, "Laundry Dryer Controls"), (0x0050, "Mode Select"),
    (0x0051, "Laundry Washer Mode"), (0x0052, "Refrigerator And Temperature Controlled Cabinet Mode"),
    (0x0053, "Laundry Washer Controls"), (0x0054, "RVC Run Mode"), (0x0055, "RVC Clean Mode"),
    (0x0056, "Temperature Control"), (0x0057, "Refrigerator Alarm"), (0x0059, "Dishwasher Mode"),
    (0x005B, "Air Quality"), (0x005C, "Smoke CO Alarm"), (0x005D, "Dishwasher Alarm"),
    (0x005E, "Microwave Oven Mode"), (0x005F, "Microwave Oven Control"),
    (0x0060, "Operational State"), (0x0061, "RVC Operational State"),
    (0x0062, "Scenes Management"), (0x0063, "Thermostat Mode"), (0x0064, "Temperature Alarm"),
    (0x0065, "Groupcast"), (0x0071, "HEPA Filter Monitoring"),
    (0x0072, "Activated Carbon Filter Monitoring"), (0x0079, "Water Tank Level Monitoring"),
    (0x0080, "Boolean State Configuration"), (0x0081, "Valve Configuration and Control"),
    (0x0090, "Electrical Power Measurement"), (0x0091, "Electrical Energy Measurement"),
    (0x0094, "Water Heater Management"), (0x0095, "Commodity Price"), (0x0097, "Messages"),
    (0x0098, "Device Energy Management"), (0x0099, "Energy EVSE"), (0x009B, "Energy Preference"),
    (0x009C, "Power Topology"), (0x009D, "Energy EVSE Mode"), (0x009E, "Water Heater Mode"),
    (0x009F, "Device Energy Management Mode"), (0x00A0, "Electrical Grid Conditions"),
    (0x00A1, "Electrical Alarm"), (0x00A2, "Electrical Distribution"),
    (0x00A3, "Electrical Protection Alarm"), (0x0101, "Door Lock"), (0x0102, "Window Covering"),
    (0x0104, "Closure Control"), (0x0105, "Closure Dimension"), (0x0150, "Service Area"),
    (0x0200, "Pump Configuration and Control"), (0x0201, "Thermostat"),
    (0x0202, "Fan Control"), (0x0204, "Thermostat User Interface Configuration"),
    (0x0205, "Humidistat"), (0x0300, "Color Control"), (0x0301, "Ballast Configuration"),
    (0x0400, "Illuminance Measurement"), (0x0402, "Temperature Measurement"),
    (0x0403, "Pressure Measurement"), (0x0404, "Flow Measurement"),
    (0x0405, "Relative Humidity Measurement"), (0x0406, "Occupancy Sensing"),
    (0x0407, "Leaf Wetness Measurement"), (0x0408, "Soil Moisture Measurement"),
    (0x040C, "Carbon Monoxide Concentration Measurement"),
    (0x040D, "Carbon Dioxide Concentration Measurement"),
    (0x0413, "Nitrogen Dioxide Concentration Measurement"),
    (0x0415, "Ozone Concentration Measurement"), (0x042A, "PM2.5 Concentration Measurement"),
    (0x042B, "Formaldehyde Concentration Measurement"), (0x042C, "PM1 Concentration Measurement"),
    (0x042D, "PM10 Concentration Measurement"),
    (0x042E, "Total Volatile Organic Compounds Concentration Measurement"),
    (0x042F, "Radon Concentration Measurement"), (0x0430, "Soil Measurement"),
    (0x0431, "Ambient Context Sensing"), (0x0432, "Ambient Sensing Union"),
    (0x0433, "Proximity Ranging"), (0x0434, "Smoke Concentration Measurement"),
    (0x0450, "Network Identity Management"), (0x0451, "Wi-Fi Network Management"),
    (0x0452, "Thread Border Router Management"), (0x0453, "Thread Network Directory"),
    (0x0455, "Commissioning Proxy"), (0x0503, "Wake On LAN"), (0x0504, "Channel"),
    (0x0505, "Target Navigator"), (0x0506, "Media Playback"), (0x0507, "Media Input"),
    (0x0508, "Low Power"), (0x0509, "Keypad Input"), (0x050A, "Content Launcher"),
    (0x050B, "Audio Output"), (0x050C, "Application Launcher"), (0x050D, "Application Basic"),
    (0x050E, "Account Login"), (0x050F, "Content Control"), (0x0510, "Content App Observer"),
    (0x0511, "Media File Management"), (0x0512, "Audio Control"), (0x0550, "Zone Management"),
    (0x0551, "Camera AV Stream Management"), (0x0552, "Camera AV Settings User Level Management"),
    (0x0553, "WebRTC Transport Provider"), (0x0554, "WebRTC Transport Requestor"),
    (0x0555, "Push AV Stream Transport"), (0x0556, "Chime"), (0x0557, "AV Analysis"),
    (0x0700, "Commodity Tariff"), (0x0750, "Ecosystem Information"),
    (0x0751, "Commissioner Control"), (0x0752, "Joint Fabric Datastore"),
    (0x0753, "Joint Fabric Administrator"), (0x0801, "TLS Certificate Management"),
    (0x0802, "TLS Client Management"), (0x0B06, "Meter Identification"),
    (0x0B07, "Commodity Metering"),
]

# Zigbee cap -> ZCL cluster id (for support flag; Zigbee map stores caps, not ids)
ZIGBEE_CAP_TO_CLUSTER = {
    "onoff": 0x0006, "level": 0x0008, "lightness": 0x0008, "ctl": 0x0300, "hsl": 0x0300,
    "color": 0x0300, "temp": 0x0402, "humidity": 0x0405, "pressure": 0x0403,
    "illuminance": 0x0400, "occupancy": 0x0406, "ias_zone": 0x0500,
    "electrical": 0x0B04, "energy": 0x0702, "power": 0x0B04, "battery": 0x0001,
}

# ---------------------------------------------------------------------------
# 3) Zigbee ZHA device IDs (HA Public Profile + Lighting/Occupancy spec)
# ---------------------------------------------------------------------------
ZIGBEE_ZHA = [
    (0x0000, "Generic On/Off Switch"), (0x0001, "Level Control Switch"),
    (0x0002, "On/Off Output"), (0x0003, "Level Controllable Output"),
    (0x0004, "Scene Selector"), (0x0005, "Configuration Tool"), (0x0006, "Remote Control"),
    (0x0007, "Combined Interface"), (0x0008, "Range Extender"), (0x0009, "Mains Power Outlet"),
    (0x000A, "Door Lock"), (0x000B, "Door Lock Controller"), (0x000C, "Simple Sensor"),
    (0x000D, "Consumption Awareness"), (0x0050, "Home Gateway"), (0x0051, "Smart plug"),
    (0x0052, "White Goods"), (0x0053, "Meter Interface"),
    (0x0100, "On/off light"), (0x0101, "Dimmable light"), (0x0102, "Color dimmable light"),
    (0x0103, "On/off light switch"), (0x0104, "Dimmer switch"), (0x0105, "Color dimmer switch"),
    (0x0106, "Light sensor"), (0x0107, "Occupancy sensor"), (0x0108, "On/off ballast"),
    (0x0109, "Dimmable ballast"), (0x010A, "On/off plug-in unit"), (0x010B, "Dimmable plug-in unit"),
    (0x010C, "Color temperature light"), (0x010D, "Extended color light"),
]
ZIGBEE_ZHA_SUPPORT = {
    "On/off light": "light", "Dimmable light": "light", "Color dimmable light": "light",
    "Color temperature light": "light", "Extended color light": "light",
    "On/off ballast": "light", "Dimmable ballast": "light", "On/off plug-in unit": "light",
    "Dimmable plug-in unit": "light", "Mains Power Outlet": "light",
    "On/off light switch": "switch", "Dimmer switch": "switch", "Color dimmer switch": "switch",
    "Generic On/Off Switch": "switch", "Level Control Switch": "switch",
    "Light sensor": "sensor_illuminance", "Occupancy sensor": "sensor_occupancy",
    "Simple Sensor": "sensor",
}
ZIGBEE_ZHA_PARAM = {
    "Door Lock": "lock_state", "Smart plug": "power",
}

# ---------------------------------------------------------------------------
# 4) BLE Mesh SIG models
# ---------------------------------------------------------------------------
BLE_MODELS = [
    (0x0000, "Config Server", "Foundation"), (0x0001, "Config Client", "Foundation"),
    (0x0002, "Health Server", "Foundation"), (0x0003, "Health Client", "Foundation"),
    (0x0004, "Remote Provisioning Server", "Foundation"),
    (0x0005, "Remote Provisioning Client", "Foundation"),
    (0x000A, "Mesh Private Beacon Server", "Foundation"),
    (0x000B, "Mesh Private Beacon Client", "Foundation"),
    (0x1000, "Generic OnOff Server", "Generic"), (0x1001, "Generic OnOff Client", "Generic"),
    (0x1002, "Generic Level Server", "Generic"), (0x1003, "Generic Level Client", "Generic"),
    (0x1004, "Generic Default Transition Time Server", "Generic"),
    (0x1005, "Generic Default Transition Time Client", "Generic"),
    (0x1006, "Generic Power OnOff Server", "Generic"),
    (0x1007, "Generic Power OnOff Setup Server", "Generic"),
    (0x1009, "Generic Power Level Server", "Generic"),
    (0x100B, "Generic Battery Server", "Generic"),
    (0x100D, "Generic Location Server", "Generic"),
    (0x1011, "Generic User Property Server", "Generic"),
    (0x1012, "Generic Admin Property Server", "Generic"),
    (0x1013, "Generic Manufacturer Property Server", "Generic"),
    (0x1014, "Generic Client Property Server", "Generic"),
    (0x1100, "Sensor Server", "Sensor"), (0x1101, "Sensor Client", "Sensor"),
    (0x1102, "Sensor Setup Server", "Sensor"),
    (0x1200, "Time Server", "Time"), (0x1201, "Time Client", "Time"),
    (0x1203, "Scene Server", "Time"), (0x1204, "Scene Client", "Time"),
    (0x1206, "Scheduler Server", "Time"), (0x1207, "Scheduler Client", "Time"),
    (0x1300, "Light Lightness Server", "Light"), (0x1301, "Light Lightness Client", "Light"),
    (0x1302, "Light Lightness Setup Server", "Light"),
    (0x1303, "Light CTL Server", "Light"), (0x1304, "Light CTL Client", "Light"),
    (0x1305, "Light CTL Setup Server", "Light"), (0x1306, "Light CTL Temperature Server", "Light"),
    (0x1307, "Light HSL Server", "Light"), (0x1308, "Light HSL Client", "Light"),
    (0x1309, "Light HSL Setup Server", "Light"), (0x130A, "Light HSL Hue Server", "Light"),
    (0x130B, "Light xyL Server", "Light"), (0x130C, "Light xyL Client", "Light"),
    (0x130D, "Light LC Server", "Light"), (0x130E, "Light LC Client", "Light"),
    (0x130F, "Light LC Setup Server", "Light"),
]
BLE_GROUP_SUPPORT = {
    "Light": "light", "Generic": "switch", "Sensor": "sensor",
}


def load_maps():
    data = {}
    for k, p in MAPS.items():
        with open(p) as f:
            data[k] = json.load(f)
    return data


def referenced_clusters(maps):
    """Cluster ids referenced by the three maps -> (matter_set, zigbee_set)."""
    matter, zigbee = set(), set()
    m = maps["matter"]
    for row in m.get("models", []):
        if "id" in row:
            matter.add(int(row["id"], 16))
    for row in m.get("controls", []):
        if "model" in row:
            matter.add(int(row["model"], 16))
    for row in m.get("sensors", []):
        if "property" in row:  # matter treats property as cluster id
            matter.add(int(row["property"], 16))
    # params cap -> cluster via models
    cap2cl = {}
    for row in m.get("models", []):
        if "cap" in row and "id" in row:
            cap2cl[row["cap"]] = int(row["id"], 16)
    for row in m.get("params", []):
        c = row.get("cap")
        if c in cap2cl:
            matter.add(cap2cl[c])

    zm = maps["zigbee"]
    caps = set()
    for sec in ("models", "params", "sensors", "controls"):
        for row in zm.get(sec, []):
            if "cap" in row:
                caps.add(row["cap"])
    for c in caps:
        if c in ZIGBEE_CAP_TO_CLUSTER:
            zigbee.add(ZIGBEE_CAP_TO_CLUSTER[c])
    return matter, zigbee


def param_set():
    """MiniHub normalized params from parameters.csv (first column, non-empty)."""
    p = os.path.join(ROOT, "components/minihub_device_widget/model/parameters.csv")
    s = set()
    with open(p) as f:
        for i, line in enumerate(f):
            if i == 0:
                continue
            name = line.split(",")[0].strip()
            if name:
                s.add(name)
    return s


def ble_group_to_minihub():
    return {"Light": "light", "Generic": "switch", "Sensor": "sensor"}


def mat_disp(ref):
    if not ref or not ref.startswith("cl"):
        return "—"
    try:
        cid = int(ref[2:], 16)
    except Exception:
        return ref
    name = cluster_name(cid)
    return f"{name} (0x{cid:04X})" if name else f"0x{cid:04X}"


def build_unified_devices(maps, params):
    """Device-type table anchored on the FULL Matter device list (105 rows).

    Columns: 适配状态 | MiniHub | BLE Mesh | Zigbee | Matter
    Every Matter device type is listed; 适配状态 flags MiniHub adaptation:
      ✅ yes  = MiniHub has a classify type for it
      🟡 param = parameter exists but no classify type yet
      ❌ no   = not supported at all (the gap)
    """
    ble_by_type, zigbee_by_type = {}, {}
    g2t = ble_group_to_minihub()
    for mid, name, grp in BLE_MODELS:
        t = g2t.get(grp)
        if t and "Server" in name:
            ble_by_type.setdefault(t, []).append(f"{name} (0x{mid:04X})")
    for name, t in ZIGBEE_ZHA_SUPPORT.items():
        zigbee_by_type.setdefault(t, []).append(name)
    rows = []
    for cid, name in MATTER_DEVICES:
        mh_type = MATTER_DEVICE_SUPPORT.get(name)
        mh_param = MATTER_DEVICE_PARAM.get(name)
        if mh_type:
            status, mh_cell = "✅ yes", mh_type
            bt = mh_type if mh_type in ble_by_type else ("sensor" if mh_type.startswith("sensor") else None)
            ble = "; ".join(ble_by_type.get(bt, [])) or "—"
            zt = mh_type if mh_type in zigbee_by_type else ("sensor" if mh_type.startswith("sensor") else None)
            zig = "; ".join(zigbee_by_type.get(zt, [])) if zt else "—"
        elif mh_param and mh_param in params:
            status, mh_cell = "🟡 param", mh_param
            ble = zig = "—"
        else:
            status, mh_cell = "❌ no", "—"
            ble = zig = "—"
        rows.append((status, mh_cell, ble, zig, f"{name} (0x{cid:04X})"))
    return rows


def build_unified_params(maps, params):
    """Param table anchored on the MiniHub normalized params (36 rows).

    Columns: 适配状态 | MiniHub | BLE Mesh | Zigbee | Matter
      ✅ yes = bound to >=1 protocol;  ❌ no = defined but unmapped (gap).
    """
    pmap = minihub_param_map(maps)
    rows = []
    for p in sorted(param_set()):
        refs = pmap.get(p, {})
        ble = refs.get("ble_mesh", "").replace("prop:", "属性 ") or "—"
        mat = mat_disp(refs.get("matter", ""))
        zig = refs.get("zigbee", "").replace("cap:", "") or "—"
        supported = any(refs.get(k) for k in ("ble_mesh", "matter", "zigbee"))
        status = "✅ yes" if supported else "❌ no"
        rows.append((status, p, ble, zig, mat))
    return rows


def main():
    maps = load_maps()
    matter_cls, zigbee_cls = referenced_clusters(maps)
    params = param_set()

    # ---- spec CSVs (FULL + support flag) ----
    with open(os.path.join(HERE, "spec_matter_device_types.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["device_id", "name", "minihub_support", "minihub_type"])
        for cid, name in MATTER_DEVICES:
            if name in MATTER_DEVICE_SUPPORT:
                w.writerow([f"0x{cid:04X}", name, "yes", MATTER_DEVICE_SUPPORT[name]])
            elif name in MATTER_DEVICE_PARAM and MATTER_DEVICE_PARAM[name] in params:
                w.writerow([f"0x{cid:04X}", name, "param", MATTER_DEVICE_PARAM[name]])
            else:
                w.writerow([f"0x{cid:04X}", name, "no", ""])

    with open(os.path.join(HERE, "spec_zcl_clusters.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["cluster_id", "name", "matter", "zigbee"])
        for cid, name in ZCL_CLUSTERS:
            w.writerow([f"0x{cid:04X}", name,
                        "yes" if cid in matter_cls else "no",
                        "yes" if cid in zigbee_cls else "no"])

    with open(os.path.join(HERE, "spec_zigbee_zha_device_ids.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["device_id", "name", "minihub_support", "minihub_type"])
        for did, name in ZIGBEE_ZHA:
            if name in ZIGBEE_ZHA_SUPPORT:
                w.writerow([f"0x{did:04X}", name, "yes", ZIGBEE_ZHA_SUPPORT[name]])
            elif name in ZIGBEE_ZHA_PARAM and ZIGBEE_ZHA_PARAM[name] in params:
                w.writerow([f"0x{did:04X}", name, "param", ZIGBEE_ZHA_PARAM[name]])
            else:
                w.writerow([f"0x{did:04X}", name, "no", ""])

    with open(os.path.join(HERE, "spec_ble_mesh_models.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["model_id", "name", "group", "minihub_support", "minihub_type"])
        for mid, name, grp in BLE_MODELS:
            sup = BLE_GROUP_SUPPORT.get(grp, "no")
            w.writerow([f"0x{mid:04X}", name, grp, "yes" if sup == "yes" else "no",
                        sup if sup == "yes" else ""])

    # ---- coverage stats ----
    md_dev = sum(1 for _, n in MATTER_DEVICES if n in MATTER_DEVICE_SUPPORT)
    md_param = sum(1 for _, n in MATTER_DEVICES
                   if n in MATTER_DEVICE_PARAM and MATTER_DEVICE_PARAM[n] in params)
    cl_matter = sum(1 for c, _ in ZCL_CLUSTERS if c in matter_cls)
    cl_zigbee = sum(1 for c, _ in ZCL_CLUSTERS if c in zigbee_cls)

    # ---- correspondence.md ----
    md = []
    md.append("# MiniHub ↔ Matter ↔ Zigbee ↔ BLE Mesh 四合一对应表（全量 + 覆盖标记）\n")
    md.append("> 生成于 2026-11-12。spec 真源已拉取并保存到本地（见 README）。\n")
    md.append("> 每行都带 MiniHub 支持标记，未支持的即“欠缺项”。\n")
    md.append("## 覆盖概览\n")
    md.append(f"- **Matter 设备类型**：MiniHub 完整支持 **{md_dev}/{len(MATTER_DEVICES)}**，另有参数级支持 {md_param} 个（参数已定义、缺 classify 设备类型）。")
    md.append(f"- **ZCL 集群（Matter/Zigbee 共用）**：MiniHub 引用 **{cl_matter}/{len(ZCL_CLUSTERS)}**（Matter 侧） / **{cl_zigbee}/{len(ZCL_CLUSTERS)}**（Zigbee 侧）。")
    md.append(f"- **Zigbee ZHA 设备**：见 `spec_zigbee_zha_device_ids.csv`。")
    md.append(f"- **BLE Mesh 模型**：见 `spec_ble_mesh_models.csv`。\n")
    md.append("> 支持标记：`yes`=MiniHub 已有 classify 设备类型；`param`=参数已定义但无设备类型；`no`=完全未支持。\n")
    md.append("> 速览：见 §1 四合一统一对照表（适配状态 / MiniHub / BLE Mesh / Zigbee / Matter；")
    md.append("> 设备表以 Matter 105 为锚、参数表以 MiniHub 36 为锚）。\n")

    dev_rows = build_unified_devices(maps, params)
    param_rows = build_unified_params(maps, params)
    md.append("## 1. 四合一统一对照表（适配状态 / MiniHub / BLE Mesh / Zigbee / Matter）\n")
    md.append("> 设备表以 **Matter 设备类型** 为锚，穷举全部 105 个 Matter Device Library 类型，")
    md.append("> 每行给出 MiniHub 适配状态与 BLE Mesh / Zigbee 的等价设备名称 —— 直接看出哪些 MiniHub")
    md.append("> 已支持、哪些欠缺。参数表以 MiniHub 归一化参数为锚（36 个），给出三协议等价引用。\n")

    md.append("### 1.1 设备类型（Matter 锚，全量 105）\n")
    md.append("| 适配状态 | MiniHub | BLE Mesh | Zigbee | Matter |")
    md.append("|---|---|---|---|---|")
    for status, mh, ble, zig, mat in dev_rows:
        md.append(f"| {status} | `{mh}` | {ble} | {zig} | {mat} |")

    md.append("\n### 1.2 参数（MiniHub 参数锚，36）\n")
    md.append("> ❌ no = 参数已定义但尚未绑定任何协议（欠缺项，待补 JSON 映射）。\n")
    md.append("| 适配状态 | MiniHub | BLE Mesh | Zigbee | Matter |")
    md.append("|---|---|---|---|---|")
    for status, mh, ble, zig, mat in param_rows:
        md.append(f"| {status} | `{mh}` | {ble} | {zig} | {mat} |")

    with open(os.path.join(HERE, "correspondence_matrix_devices.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["adapt_status", "minihub", "ble_mesh", "zigbee", "matter"])
        for status, mh, ble, zig, mat in dev_rows:
            w.writerow([status, mh, ble, zig, mat])
    with open(os.path.join(HERE, "correspondence_matrix_params.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["adapt_status", "minihub", "ble_mesh", "zigbee", "matter"])
        for status, mh, ble, zig, mat in param_rows:
            w.writerow([status, mh, ble, zig, mat])

    md.append("\n## 2. Matter 设备类型（全量 105，带支持标记）\n")
    md.append("| Device ID | Name | MiniHub | 类型 |")
    md.append("|---|---|---|---|")
    for cid, name in MATTER_DEVICES:
        if name in MATTER_DEVICE_SUPPORT:
            md.append(f"| 0x{cid:04X} | {name} | ✅ yes | {MATTER_DEVICE_SUPPORT[name]} |")
        elif name in MATTER_DEVICE_PARAM and MATTER_DEVICE_PARAM[name] in params:
            md.append(f"| 0x{cid:04X} | {name} | 🟡 param | {MATTER_DEVICE_PARAM[name]} |")
        else:
            md.append(f"| 0x{cid:04X} | {name} | ❌ no |  |")

    md.append("\n## 3. ZCL 集群（全量 156，Matter/Zigbee 共用，带引用标记）\n")
    md.append("| Cluster ID | Name | Matter | Zigbee |")
    md.append("|---|---|---|---|")
    for cid, name in ZCL_CLUSTERS:
        m = "✅" if cid in matter_cls else "—"
        z = "✅" if cid in zigbee_cls else "—"
        md.append(f"| 0x{cid:04X} | {name} | {m} | {z} |")

    md.append("\n## 4. Zigbee ZHA 设备（带支持标记）\n")
    md.append("| Device ID | Name | MiniHub | 类型 |")
    md.append("|---|---|---|---|")
    for did, name in ZIGBEE_ZHA:
        if name in ZIGBEE_ZHA_SUPPORT:
            md.append(f"| 0x{did:04X} | {name} | ✅ yes | {ZIGBEE_ZHA_SUPPORT[name]} |")
        elif name in ZIGBEE_ZHA_PARAM and ZIGBEE_ZHA_PARAM[name] in params:
            md.append(f"| 0x{did:04X} | {name} | 🟡 param | {ZIGBEE_ZHA_PARAM[name]} |")
        else:
            md.append(f"| 0x{did:04X} | {name} | ❌ no |  |")

    md.append("\n## 5. BLE Mesh SIG 模型（带支持标记）\n")
    md.append("| Model ID | Name | Group | MiniHub |")
    md.append("|---|---|---|---|")
    for mid, name, grp in BLE_MODELS:
        sup = BLE_GROUP_SUPPORT.get(grp, "no")
        flag = "✅ yes" if sup == "yes" else "❌ no"
        md.append(f"| 0x{mid:04X} | {name} | {grp} | {flag} |")

    md.append("\n## 6. 参数对应（来自三张能力图，MiniHub 已落地部分）\n")
    md.append("| MiniHub 参数 | Matter | Zigbee | BLE Mesh | spec 集群 |")
    md.append("|---|---|---|---|---|")
    pmap = minihub_param_map(maps)
    for p in sorted(pmap):
        refs = pmap[p]
        m = refs.get("matter", "")
        z = refs.get("zigbee", "")
        b = refs.get("ble_mesh", "")
        cl = cluster_from_ref(m) or cluster_from_ref(z) or cluster_from_ref(b)
        clname = cluster_name(cl)
        md.append(f"| `{p}` | {m} | {z} | {b} | {clname} |")

    md.append("\n## 7. 已知缺陷（建表时发现，需修正 — 在 Matter `sensors`）\n")
    md.append("- `motion`(0x002F) 在 matter_map.json 的 `sensors` 中 `property=0x002F` 被当作集群 id 解析，")
    md.append("  但 0x002F 是 *Power Source* 集群，并非运动；ZCL/Matter 无独立 motion 集群，运动经")
    md.append("  OccupancySensing(0x0406) 的 occupancy 位图 bit1 表达。建议 motion 改指 0x0406（与 occupancy")
    md.append("  同集群，由解码层按位区分）或 Matter 下不单独支持 motion。")
    md.append("- `co2`(0x040D) 经核对为**合法** ZCL 集群 *Carbon Dioxide Concentration Measurement*")
    md.append("  （与 BLE 的 CO2 属性 id 0x040D 恰好同号、不同命名空间），故 co2 在 Matter 下可正常解析，非缺陷。")
    md.append("- `property` 字段语义按协议不同：BLE=`BLE Property ID`，Matter=`Matter 集群 id`。\n")

    with open(os.path.join(HERE, "correspondence.md"), "w") as f:
        f.write("\n".join(md))

    # ---- README ----
    readme = """# spec_correspondence

四合一对应表（BLE Mesh / Matter / Zigbee / MiniHub）的 spec 真源与生成脚本。

## 数据来源（2026-11-12 拉取，官方权威）
- Matter 设备类型（105）：connectedhomeip 自动生成的
  `ids_and_codes/spec_device_types.html`（每个含 hex ID + 名）。
- Zigbee/Matter ZCL 集群（159）：connectedhomeip 自动生成的
  `ids_and_codes/spec_clusters.html`（每个含 hex ID + 名）。这是 Matter 与 Zigbee
  共用的 ZCL 集群集合。
- Zigbee ZHA 设备 ID：Zigbee HA Public Profile (NXP) + Zigbee Lighting &
  Occupancy Device Spec (CSA) 片段。
- BLE Mesh 模型：SIG Model ID 表（cfanz / Silabs / Wikiwand）。

## 文件
- spec_matter_device_types.csv / spec_zcl_clusters.csv /
  spec_zigbee_zha_device_ids.csv / spec_ble_mesh_models.csv — 全量 spec 真源
  + MiniHub 支持标记（yes / param / no；集群表为 matter/zigbee 引用标记）。
- correspondence.md — 人读全量四合一表（§1 四合一统一对照表按 MiniHub 实体对齐；
  后续 §2–§7 为分协议全量明细 + 覆盖概览与缺口）。
- correspondence_matrix_devices.csv — §1.1 设备表的机器可读版（Matter 锚，105 行；adapt_status / minihub / ble_mesh / zigbee / matter）。
- correspondence_matrix_params.csv — §1.2 参数表的机器可读版（MiniHub 锚，36 行；同 5 列）。
- correspondence_params.csv / correspondence_devices.csv — 机器可读（MiniHub 侧）。
- minihub_mapping_raw.csv — 三张能力图解析出的 (协议, 种类, minihub键, 原生引用)。

## 如何扩展
1. 给三张能力图补 `classify` / `params` / `sensors` / `controls`。
2. 重跑 `python3 tools/spec_correspondence/build_correspondence.py`。
3. spec 真源如需更新：把 connectedhomeip 的两个 HTML 重新解析，替换脚本里的
   MATTER_DEVICES / ZCL_CLUSTERS 列表即可。
"""
    with open(os.path.join(HERE, "README.md"), "w") as f:
        f.write(readme)

    print("matter devices:", len(MATTER_DEVICES), "zigbee clusters:", len(ZCL_CLUSTERS),
          "zha devices:", len(ZIGBEE_ZHA), "ble models:", len(BLE_MODELS))
    print(f"coverage: matter dev yes={md_dev} param={md_param}; clusters matter={cl_matter} zigbee={cl_zigbee}")


def minihub_param_map(maps):
    """param -> {proto: native_ref}. See build script header for `sensors` semantics."""
    out = {}
    for proto, m in maps.items():
        for sec in ("params", "sensors", "controls"):
            for row in m.get(sec, []):
                p = row.get("param")
                if not p:
                    continue
                if sec == "sensors":
                    if "property" in row:
                        ref = ("cl" + row["property"]) if proto == "matter" else ("prop:" + row["property"])
                    else:
                        ref = ""
                elif "cluster" in row and "attribute" in row:
                    ref = f"cl{row['cluster']}/at{row['attribute']}"
                elif "cluster" in row and "command" in row:
                    ref = f"cl{row['cluster']}/cmd{row['command']}"
                elif "cap" in row:
                    ref = f"cap:{row['cap']}"
                elif "prop" in row:
                    ref = f"prop:{row['prop']}"
                else:
                    ref = ""
                out.setdefault(p, {})[proto] = ref
    return out


def cluster_from_ref(ref):
    if ref and ref.startswith("cl"):
        try:
            return int(ref[2:], 16)
        except Exception:
            return None
    return None


def cluster_name(cid):
    if cid is None:
        return ""
    return dict((c, n) for c, n in ZCL_CLUSTERS).get(cid, "")


if __name__ == "__main__":
    main()

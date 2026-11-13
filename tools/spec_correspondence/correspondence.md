# MiniHub ↔ Matter ↔ Zigbee ↔ BLE Mesh 四合一对应表（全量 + 覆盖标记）

> 生成于 2026-11-12。spec 真源已拉取并保存到本地（见 README）。

> 每行都带 MiniHub 支持标记，未支持的即“欠缺项”。

## 覆盖概览

- **Matter 设备类型**：MiniHub 完整支持 **18/105**，另有参数级支持 4 个（参数已定义、缺 classify 设备类型）。
- **ZCL 集群（Matter/Zigbee 共用）**：MiniHub 引用 **10/156**（Matter 侧） / **3/156**（Zigbee 侧）。
- **Zigbee ZHA 设备**：见 `spec_zigbee_zha_device_ids.csv`。
- **BLE Mesh 模型**：见 `spec_ble_mesh_models.csv`。

> 支持标记：`yes`=MiniHub 已有 classify 设备类型；`param`=参数已定义但无设备类型；`no`=完全未支持。

> 速览：见 §1 四合一统一对照表（适配状态 / MiniHub / BLE Mesh / Zigbee / Matter；
> 设备表以 Matter 105 为锚、参数表以 MiniHub 36 为锚）。

## 1. 四合一统一对照表（适配状态 / MiniHub / BLE Mesh / Zigbee / Matter）

> 设备表以 **Matter 设备类型** 为锚，穷举全部 105 个 Matter Device Library 类型，
> 每行给出 MiniHub 适配状态与 BLE Mesh / Zigbee 的等价设备名称 —— 直接看出哪些 MiniHub
> 已支持、哪些欠缺。参数表以 MiniHub 归一化参数为锚（36 个），给出三协议等价引用。

### 1.1 设备类型（Matter 锚，全量 105）

| 适配状态 | MiniHub | BLE Mesh | Zigbee | Matter |
|---|---|---|---|---|
| ❌ no | `—` | — | — | Door Lock (0x000A) |
| ❌ no | `—` | — | — | Door Lock Controller (0x000B) |
| ❌ no | `—` | — | — | Aggregator (0x000E) |
| ✅ yes | `switch` | Generic OnOff Server (0x1000); Generic Level Server (0x1002); Generic Default Transition Time Server (0x1004); Generic Power OnOff Server (0x1006); Generic Power OnOff Setup Server (0x1007); Generic Power Level Server (0x1009); Generic Battery Server (0x100B); Generic Location Server (0x100D); Generic User Property Server (0x1011); Generic Admin Property Server (0x1012); Generic Manufacturer Property Server (0x1013); Generic Client Property Server (0x1014) | On/off light switch; Dimmer switch; Color dimmer switch; Generic On/Off Switch; Level Control Switch | Generic Switch (0x000F) |
| ❌ no | `—` | — | — | Power Source (0x0011) |
| ❌ no | `—` | — | — | OTA Requestor (0x0012) |
| ❌ no | `—` | — | — | Bridged Node (0x0013) |
| ❌ no | `—` | — | — | OTA Provider (0x0014) |
| 🟡 param | `contact` | — | — | Contact Sensor (0x0015) |
| ❌ no | `—` | — | — | Root Node (0x0016) |
| ❌ no | `—` | — | — | Solar Power (0x0017) |
| ❌ no | `—` | — | — | Battery Storage (0x0018) |
| ❌ no | `—` | — | — | Secondary Network Interface (0x0019) |
| ❌ no | `—` | — | — | Streaming Audio Player (0x0020) |
| ❌ no | `—` | — | — | Casting Audio Player (0x0021) |
| ❌ no | `—` | — | — | Speaker (0x0022) |
| ❌ no | `—` | — | — | Casting Video Player (0x0023) |
| ❌ no | `—` | — | — | Content App (0x0024) |
| ❌ no | `—` | — | — | Mode Select (0x0027) |
| ❌ no | `—` | — | — | Basic Video Player (0x0028) |
| ❌ no | `—` | — | — | Casting Video Client (0x0029) |
| ❌ no | `—` | — | — | Video Remote Control (0x002A) |
| ❌ no | `—` | — | — | Fan (0x002B) |
| ✅ yes | `sensor_air_quality` | Sensor Server (0x1100); Sensor Setup Server (0x1102) | Simple Sensor | Air Quality Sensor (0x002C) |
| ❌ no | `—` | — | — | Air Purifier (0x002D) |
| ❌ no | `—` | — | — | Irrigation System (0x0040) |
| ❌ no | `—` | — | — | Water Freeze Detector (0x0041) |
| ❌ no | `—` | — | — | Water Valve (0x0042) |
| 🟡 param | `leak` | — | — | Water Leak Detector (0x0043) |
| ❌ no | `—` | — | — | Rain Sensor (0x0044) |
| ❌ no | `—` | — | — | Soil Sensor (0x0045) |
| ❌ no | `—` | — | — | Refrigerator (0x0070) |
| ❌ no | `—` | — | — | Temperature Controlled Cabinet (0x0071) |
| ❌ no | `—` | — | — | Room Air Conditioner (0x0072) |
| ❌ no | `—` | — | — | Laundry Washer (0x0073) |
| ❌ no | `—` | — | — | Robotic Vacuum Cleaner (0x0074) |
| ❌ no | `—` | — | — | Dishwasher (0x0075) |
| 🟡 param | `smoke` | — | — | Smoke CO Alarm (0x0076) |
| ❌ no | `—` | — | — | Cook Surface (0x0077) |
| ❌ no | `—` | — | — | Cooktop (0x0078) |
| ❌ no | `—` | — | — | Microwave Oven (0x0079) |
| ❌ no | `—` | — | — | Extractor Hood (0x007A) |
| ❌ no | `—` | — | — | Oven (0x007B) |
| ❌ no | `—` | — | — | Laundry Dryer (0x007C) |
| ❌ no | `—` | — | — | Humidity Conditioner (0x007D) |
| ❌ no | `—` | — | — | Network Infrastructure Manager (0x0090) |
| ❌ no | `—` | — | — | Thread Border Router (0x0091) |
| ❌ no | `—` | — | — | Commissioning By Proxy (0x0092) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | On/Off Light (0x0100) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | Dimmable Light (0x0101) |
| ✅ yes | `switch` | Generic OnOff Server (0x1000); Generic Level Server (0x1002); Generic Default Transition Time Server (0x1004); Generic Power OnOff Server (0x1006); Generic Power OnOff Setup Server (0x1007); Generic Power Level Server (0x1009); Generic Battery Server (0x100B); Generic Location Server (0x100D); Generic User Property Server (0x1011); Generic Admin Property Server (0x1012); Generic Manufacturer Property Server (0x1013); Generic Client Property Server (0x1014) | On/off light switch; Dimmer switch; Color dimmer switch; Generic On/Off Switch; Level Control Switch | On/Off Light Switch (0x0103) |
| ✅ yes | `switch` | Generic OnOff Server (0x1000); Generic Level Server (0x1002); Generic Default Transition Time Server (0x1004); Generic Power OnOff Server (0x1006); Generic Power OnOff Setup Server (0x1007); Generic Power Level Server (0x1009); Generic Battery Server (0x100B); Generic Location Server (0x100D); Generic User Property Server (0x1011); Generic Admin Property Server (0x1012); Generic Manufacturer Property Server (0x1013); Generic Client Property Server (0x1014) | On/off light switch; Dimmer switch; Color dimmer switch; Generic On/Off Switch; Level Control Switch | Dimmer Switch (0x0104) |
| ✅ yes | `switch` | Generic OnOff Server (0x1000); Generic Level Server (0x1002); Generic Default Transition Time Server (0x1004); Generic Power OnOff Server (0x1006); Generic Power OnOff Setup Server (0x1007); Generic Power Level Server (0x1009); Generic Battery Server (0x100B); Generic Location Server (0x100D); Generic User Property Server (0x1011); Generic Admin Property Server (0x1012); Generic Manufacturer Property Server (0x1013); Generic Client Property Server (0x1014) | On/off light switch; Dimmer switch; Color dimmer switch; Generic On/Off Switch; Level Control Switch | Color Dimmer Switch (0x0105) |
| ✅ yes | `sensor_illuminance` | Sensor Server (0x1100); Sensor Setup Server (0x1102) | Light sensor | Light Sensor (0x0106) |
| ✅ yes | `sensor_occupancy` | Sensor Server (0x1100); Sensor Setup Server (0x1102) | Occupancy sensor | Occupancy Sensor (0x0107) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | On/Off Plug-in Unit (0x010A) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | Dimmable Plug-In Unit (0x010B) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | Color Temperature Light (0x010C) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | Extended Color Light (0x010D) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | Mounted On/Off Control (0x010F) |
| ✅ yes | `light` | Light Lightness Server (0x1300); Light Lightness Setup Server (0x1302); Light CTL Server (0x1303); Light CTL Setup Server (0x1305); Light CTL Temperature Server (0x1306); Light HSL Server (0x1307); Light HSL Setup Server (0x1309); Light HSL Hue Server (0x130A); Light xyL Server (0x130B); Light LC Server (0x130D); Light LC Setup Server (0x130F) | On/off light; Dimmable light; Color dimmable light; Color temperature light; Extended color light; On/off ballast; Dimmable ballast; On/off plug-in unit; Dimmable plug-in unit; Mains Power Outlet | Mounted Dimmable Load Control (0x0110) |
| ❌ no | `—` | — | — | Joint Fabric Administrator (0x0130) |
| ❌ no | `—` | — | — | Intercom (0x0140) |
| ❌ no | `—` | — | — | Audio Doorbell (0x0141) |
| ❌ no | `—` | — | — | Camera (0x0142) |
| ❌ no | `—` | — | — | Video Doorbell (0x0143) |
| ❌ no | `—` | — | — | Floodlight Camera (0x0144) |
| ❌ no | `—` | — | — | Snapshot Camera (0x0145) |
| ❌ no | `—` | — | — | Chime (0x0146) |
| ❌ no | `—` | — | — | Camera Controller (0x0147) |
| ❌ no | `—` | — | — | Doorbell (0x0148) |
| ❌ no | `—` | — | — | AV Analysis Node (0x0149) |
| ❌ no | `—` | — | — | Ambient Context Sensor (0x0150) |
| ❌ no | `—` | — | — | Proximity Ranger (0x0152) |
| ❌ no | `—` | — | — | Window Covering (0x0202) |
| ❌ no | `—` | — | — | Window Covering Controller (0x0203) |
| ❌ no | `—` | — | — | Closure (0x0230) |
| ❌ no | `—` | — | — | Closure Panel (0x0231) |
| ❌ no | `—` | — | — | Closure Controller (0x023E) |
| ❌ no | `—` | — | — | Heating/Cooling Unit (0x0300) |
| ❌ no | `—` | — | — | Thermostat (0x0301) |
| ✅ yes | `sensor_temp_humidity` | Sensor Server (0x1100); Sensor Setup Server (0x1102) | Simple Sensor | Temperature Sensor (0x0302) |
| ❌ no | `—` | — | — | Pump (0x0303) |
| ❌ no | `—` | — | — | Pump Controller (0x0304) |
| 🟡 param | `pressure_hpa` | — | — | Pressure Sensor (0x0305) |
| ❌ no | `—` | — | — | Flow Sensor (0x0306) |
| ✅ yes | `sensor_temp_humidity` | Sensor Server (0x1100); Sensor Setup Server (0x1102) | Simple Sensor | Humidity Sensor (0x0307) |
| ❌ no | `—` | — | — | Heat Pump (0x0309) |
| ❌ no | `—` | — | — | Thermostat Controller (0x030A) |
| ❌ no | `—` | — | — | Energy EVSE (0x050C) |
| ❌ no | `—` | — | — | Device Energy Management (0x050D) |
| ❌ no | `—` | — | — | Water Heater (0x050F) |
| ❌ no | `—` | — | — | Electrical Sensor (0x0510) |
| ❌ no | `—` | — | — | Electrical Utility Meter (0x0511) |
| ❌ no | `—` | — | — | Meter Reference Point (0x0512) |
| ❌ no | `—` | — | — | Electrical Energy Tariff (0x0513) |
| ❌ no | `—` | — | — | Electrical Meter (0x0514) |
| ❌ no | `—` | — | — | Arc Fault Circuit Interrupter (0x0515) |
| ❌ no | `—` | — | — | Electrical Circuit Breaker (0x0516) |
| ❌ no | `—` | — | — | Electrical Distribution Enclosure (0x0517) |
| ❌ no | `—` | — | — | Electrical Surge Protector (0x0518) |
| ❌ no | `—` | — | — | Residual Current Circuit Breaker (0x0519) |
| ❌ no | `—` | — | — | Auxiliary Load Switch (0x0520) |
| ✅ yes | `switch` | Generic OnOff Server (0x1000); Generic Level Server (0x1002); Generic Default Transition Time Server (0x1004); Generic Power OnOff Server (0x1006); Generic Power OnOff Setup Server (0x1007); Generic Power Level Server (0x1009); Generic Battery Server (0x100B); Generic Location Server (0x100D); Generic User Property Server (0x1011); Generic Admin Property Server (0x1012); Generic Manufacturer Property Server (0x1013); Generic Client Property Server (0x1014) | On/off light switch; Dimmer switch; Color dimmer switch; Generic On/Off Switch; Level Control Switch | Control Bridge (0x0840) |
| ❌ no | `—` | — | — | On/Off Sensor (0x0850) |

### 1.2 参数（MiniHub 参数锚，36）

> ❌ no = 参数已定义但尚未绑定任何协议（欠缺项，待补 JSON 映射）。

| 适配状态 | MiniHub | BLE Mesh | Zigbee | Matter |
|---|---|---|---|---|
| ❌ no | `battery` | — | — | — |
| ❌ no | `battery_low` | — | — | — |
| ❌ no | `brightness` | — | — | — |
| ❌ no | `cct` | — | — | — |
| ✅ yes | `co2` | 属性 0x0077 | — | Carbon Dioxide Concentration Measurement (0x040D) |
| ❌ no | `contact` | — | — | — |
| ❌ no | `current_a` | — | — | — |
| ❌ no | `energy_wh` | — | — | — |
| ❌ no | `fault` | — | — | — |
| ❌ no | `hue` | — | — | — |
| ✅ yes | `humidity` | 属性 0x0076 | — | Relative Humidity Measurement (0x0405) |
| ✅ yes | `illuminance` | 属性 0x0055 | — | Illuminance Measurement (0x0400) |
| ❌ no | `indicator` | — | — | — |
| ❌ no | `intensity` | — | — | — |
| ❌ no | `leak` | — | — | — |
| ✅ yes | `motion` | 属性 0x0042 | — | Power Source (0x002F) |
| ❌ no | `mute` | — | — | — |
| ❌ no | `name` | — | — | — |
| ✅ yes | `occupancy` | 属性 0x004d | — | Occupancy Sensing (0x0406) |
| ❌ no | `play_next` | — | — | — |
| ❌ no | `play_pause` | — | — | — |
| ❌ no | `play_prev` | — | — | — |
| ❌ no | `play_resume` | — | — | — |
| ❌ no | `pm25` | — | — | — |
| ❌ no | `power` | — | — | — |
| ❌ no | `power_w` | — | — | — |
| ✅ yes | `pressure_hpa` | 属性 0x00a9 | — | Pressure Measurement (0x0403) |
| ❌ no | `saturation` | — | — | — |
| ❌ no | `smoke` | — | — | — |
| ❌ no | `tamper` | — | — | — |
| ✅ yes | `temperature` | 属性 0x0075 | — | Temperature Measurement (0x0402) |
| ❌ no | `unmute` | — | — | — |
| ❌ no | `voltage_v` | — | — | — |
| ❌ no | `volume` | — | — | — |
| ❌ no | `wifi_signal` | — | — | — |
| ❌ no | `zone_status` | — | — | — |

## 2. Matter 设备类型（全量 105，带支持标记）

| Device ID | Name | MiniHub | 类型 |
|---|---|---|---|
| 0x000A | Door Lock | ❌ no |  |
| 0x000B | Door Lock Controller | ❌ no |  |
| 0x000E | Aggregator | ❌ no |  |
| 0x000F | Generic Switch | ✅ yes | switch |
| 0x0011 | Power Source | ❌ no |  |
| 0x0012 | OTA Requestor | ❌ no |  |
| 0x0013 | Bridged Node | ❌ no |  |
| 0x0014 | OTA Provider | ❌ no |  |
| 0x0015 | Contact Sensor | 🟡 param | contact |
| 0x0016 | Root Node | ❌ no |  |
| 0x0017 | Solar Power | ❌ no |  |
| 0x0018 | Battery Storage | ❌ no |  |
| 0x0019 | Secondary Network Interface | ❌ no |  |
| 0x0020 | Streaming Audio Player | ❌ no |  |
| 0x0021 | Casting Audio Player | ❌ no |  |
| 0x0022 | Speaker | ❌ no |  |
| 0x0023 | Casting Video Player | ❌ no |  |
| 0x0024 | Content App | ❌ no |  |
| 0x0027 | Mode Select | ❌ no |  |
| 0x0028 | Basic Video Player | ❌ no |  |
| 0x0029 | Casting Video Client | ❌ no |  |
| 0x002A | Video Remote Control | ❌ no |  |
| 0x002B | Fan | ❌ no |  |
| 0x002C | Air Quality Sensor | ✅ yes | sensor_air_quality |
| 0x002D | Air Purifier | ❌ no |  |
| 0x0040 | Irrigation System | ❌ no |  |
| 0x0041 | Water Freeze Detector | ❌ no |  |
| 0x0042 | Water Valve | ❌ no |  |
| 0x0043 | Water Leak Detector | 🟡 param | leak |
| 0x0044 | Rain Sensor | ❌ no |  |
| 0x0045 | Soil Sensor | ❌ no |  |
| 0x0070 | Refrigerator | ❌ no |  |
| 0x0071 | Temperature Controlled Cabinet | ❌ no |  |
| 0x0072 | Room Air Conditioner | ❌ no |  |
| 0x0073 | Laundry Washer | ❌ no |  |
| 0x0074 | Robotic Vacuum Cleaner | ❌ no |  |
| 0x0075 | Dishwasher | ❌ no |  |
| 0x0076 | Smoke CO Alarm | 🟡 param | smoke |
| 0x0077 | Cook Surface | ❌ no |  |
| 0x0078 | Cooktop | ❌ no |  |
| 0x0079 | Microwave Oven | ❌ no |  |
| 0x007A | Extractor Hood | ❌ no |  |
| 0x007B | Oven | ❌ no |  |
| 0x007C | Laundry Dryer | ❌ no |  |
| 0x007D | Humidity Conditioner | ❌ no |  |
| 0x0090 | Network Infrastructure Manager | ❌ no |  |
| 0x0091 | Thread Border Router | ❌ no |  |
| 0x0092 | Commissioning By Proxy | ❌ no |  |
| 0x0100 | On/Off Light | ✅ yes | light |
| 0x0101 | Dimmable Light | ✅ yes | light |
| 0x0103 | On/Off Light Switch | ✅ yes | switch |
| 0x0104 | Dimmer Switch | ✅ yes | switch |
| 0x0105 | Color Dimmer Switch | ✅ yes | switch |
| 0x0106 | Light Sensor | ✅ yes | sensor_illuminance |
| 0x0107 | Occupancy Sensor | ✅ yes | sensor_occupancy |
| 0x010A | On/Off Plug-in Unit | ✅ yes | light |
| 0x010B | Dimmable Plug-In Unit | ✅ yes | light |
| 0x010C | Color Temperature Light | ✅ yes | light |
| 0x010D | Extended Color Light | ✅ yes | light |
| 0x010F | Mounted On/Off Control | ✅ yes | light |
| 0x0110 | Mounted Dimmable Load Control | ✅ yes | light |
| 0x0130 | Joint Fabric Administrator | ❌ no |  |
| 0x0140 | Intercom | ❌ no |  |
| 0x0141 | Audio Doorbell | ❌ no |  |
| 0x0142 | Camera | ❌ no |  |
| 0x0143 | Video Doorbell | ❌ no |  |
| 0x0144 | Floodlight Camera | ❌ no |  |
| 0x0145 | Snapshot Camera | ❌ no |  |
| 0x0146 | Chime | ❌ no |  |
| 0x0147 | Camera Controller | ❌ no |  |
| 0x0148 | Doorbell | ❌ no |  |
| 0x0149 | AV Analysis Node | ❌ no |  |
| 0x0150 | Ambient Context Sensor | ❌ no |  |
| 0x0152 | Proximity Ranger | ❌ no |  |
| 0x0202 | Window Covering | ❌ no |  |
| 0x0203 | Window Covering Controller | ❌ no |  |
| 0x0230 | Closure | ❌ no |  |
| 0x0231 | Closure Panel | ❌ no |  |
| 0x023E | Closure Controller | ❌ no |  |
| 0x0300 | Heating/Cooling Unit | ❌ no |  |
| 0x0301 | Thermostat | ❌ no |  |
| 0x0302 | Temperature Sensor | ✅ yes | sensor_temp_humidity |
| 0x0303 | Pump | ❌ no |  |
| 0x0304 | Pump Controller | ❌ no |  |
| 0x0305 | Pressure Sensor | 🟡 param | pressure_hpa |
| 0x0306 | Flow Sensor | ❌ no |  |
| 0x0307 | Humidity Sensor | ✅ yes | sensor_temp_humidity |
| 0x0309 | Heat Pump | ❌ no |  |
| 0x030A | Thermostat Controller | ❌ no |  |
| 0x050C | Energy EVSE | ❌ no |  |
| 0x050D | Device Energy Management | ❌ no |  |
| 0x050F | Water Heater | ❌ no |  |
| 0x0510 | Electrical Sensor | ❌ no |  |
| 0x0511 | Electrical Utility Meter | ❌ no |  |
| 0x0512 | Meter Reference Point | ❌ no |  |
| 0x0513 | Electrical Energy Tariff | ❌ no |  |
| 0x0514 | Electrical Meter | ❌ no |  |
| 0x0515 | Arc Fault Circuit Interrupter | ❌ no |  |
| 0x0516 | Electrical Circuit Breaker | ❌ no |  |
| 0x0517 | Electrical Distribution Enclosure | ❌ no |  |
| 0x0518 | Electrical Surge Protector | ❌ no |  |
| 0x0519 | Residual Current Circuit Breaker | ❌ no |  |
| 0x0520 | Auxiliary Load Switch | ❌ no |  |
| 0x0840 | Control Bridge | ✅ yes | switch |
| 0x0850 | On/Off Sensor | ❌ no |  |

## 3. ZCL 集群（全量 156，Matter/Zigbee 共用，带引用标记）

| Cluster ID | Name | Matter | Zigbee |
|---|---|---|---|
| 0x0003 | Identify | — | — |
| 0x0004 | Groups | — | — |
| 0x0005 | Scenes | — | — |
| 0x0006 | On/Off | ✅ | ✅ |
| 0x0008 | Level Control | ✅ | — |
| 0x001C | Pulse Width Modulation | — | — |
| 0x001D | Descriptor | — | — |
| 0x001E | Binding | — | — |
| 0x001F | Access Control | — | — |
| 0x0025 | Actions | — | — |
| 0x0028 | Basic Information | — | — |
| 0x0029 | OTA Software Update Provider | — | — |
| 0x002A | OTA Software Update Requestor | — | — |
| 0x002B | Localization Configuration | — | — |
| 0x002C | Time Format Localization | — | — |
| 0x002D | Unit Localization | — | — |
| 0x002E | Power Source Configuration | — | — |
| 0x002F | Power Source | ✅ | — |
| 0x0030 | General Commissioning | — | — |
| 0x0031 | Network Commissioning | — | — |
| 0x0032 | Diagnostic Logs | — | — |
| 0x0033 | General Diagnostics | — | — |
| 0x0034 | Software Diagnostics | — | — |
| 0x0035 | Thread Network Diagnostics | — | — |
| 0x0036 | Wi-Fi Network Diagnostics | — | — |
| 0x0037 | Ethernet Network Diagnostics | — | — |
| 0x0038 | Time Synchronization | — | — |
| 0x0039 | Bridged Device Basic Information | — | — |
| 0x003B | Switch | — | — |
| 0x003C | Administrator Commissioning | — | — |
| 0x003E | Operational Credentials | — | — |
| 0x003F | Group Key Management | — | — |
| 0x0040 | Fixed Label | — | — |
| 0x0041 | User Label | — | — |
| 0x0042 | Proxy Configuration | — | — |
| 0x0043 | Proxy Discovery | — | — |
| 0x0044 | Valid Proxies | — | — |
| 0x0045 | Boolean State | — | — |
| 0x0046 | ICD Management | — | — |
| 0x0048 | Oven Cavity Operational State | — | — |
| 0x0049 | Oven Mode | — | — |
| 0x004A | Laundry Dryer Controls | — | — |
| 0x0050 | Mode Select | — | — |
| 0x0051 | Laundry Washer Mode | — | — |
| 0x0052 | Refrigerator And Temperature Controlled Cabinet Mode | — | — |
| 0x0053 | Laundry Washer Controls | — | — |
| 0x0054 | RVC Run Mode | — | — |
| 0x0055 | RVC Clean Mode | — | — |
| 0x0056 | Temperature Control | — | — |
| 0x0057 | Refrigerator Alarm | — | — |
| 0x0059 | Dishwasher Mode | — | — |
| 0x005B | Air Quality | — | — |
| 0x005C | Smoke CO Alarm | — | — |
| 0x005D | Dishwasher Alarm | — | — |
| 0x005E | Microwave Oven Mode | — | — |
| 0x005F | Microwave Oven Control | — | — |
| 0x0060 | Operational State | — | — |
| 0x0061 | RVC Operational State | — | — |
| 0x0062 | Scenes Management | — | — |
| 0x0063 | Thermostat Mode | — | — |
| 0x0064 | Temperature Alarm | — | — |
| 0x0065 | Groupcast | — | — |
| 0x0071 | HEPA Filter Monitoring | — | — |
| 0x0072 | Activated Carbon Filter Monitoring | — | — |
| 0x0079 | Water Tank Level Monitoring | — | — |
| 0x0080 | Boolean State Configuration | — | — |
| 0x0081 | Valve Configuration and Control | — | — |
| 0x0090 | Electrical Power Measurement | — | — |
| 0x0091 | Electrical Energy Measurement | — | — |
| 0x0094 | Water Heater Management | — | — |
| 0x0095 | Commodity Price | — | — |
| 0x0097 | Messages | — | — |
| 0x0098 | Device Energy Management | — | — |
| 0x0099 | Energy EVSE | — | — |
| 0x009B | Energy Preference | — | — |
| 0x009C | Power Topology | — | — |
| 0x009D | Energy EVSE Mode | — | — |
| 0x009E | Water Heater Mode | — | — |
| 0x009F | Device Energy Management Mode | — | — |
| 0x00A0 | Electrical Grid Conditions | — | — |
| 0x00A1 | Electrical Alarm | — | — |
| 0x00A2 | Electrical Distribution | — | — |
| 0x00A3 | Electrical Protection Alarm | — | — |
| 0x0101 | Door Lock | — | — |
| 0x0102 | Window Covering | — | — |
| 0x0104 | Closure Control | — | — |
| 0x0105 | Closure Dimension | — | — |
| 0x0150 | Service Area | — | — |
| 0x0200 | Pump Configuration and Control | — | — |
| 0x0201 | Thermostat | — | — |
| 0x0202 | Fan Control | — | — |
| 0x0204 | Thermostat User Interface Configuration | — | — |
| 0x0205 | Humidistat | — | — |
| 0x0300 | Color Control | ✅ | — |
| 0x0301 | Ballast Configuration | — | — |
| 0x0400 | Illuminance Measurement | ✅ | — |
| 0x0402 | Temperature Measurement | ✅ | ✅ |
| 0x0403 | Pressure Measurement | ✅ | — |
| 0x0404 | Flow Measurement | — | — |
| 0x0405 | Relative Humidity Measurement | ✅ | ✅ |
| 0x0406 | Occupancy Sensing | ✅ | — |
| 0x0407 | Leaf Wetness Measurement | — | — |
| 0x0408 | Soil Moisture Measurement | — | — |
| 0x040C | Carbon Monoxide Concentration Measurement | — | — |
| 0x040D | Carbon Dioxide Concentration Measurement | ✅ | — |
| 0x0413 | Nitrogen Dioxide Concentration Measurement | — | — |
| 0x0415 | Ozone Concentration Measurement | — | — |
| 0x042A | PM2.5 Concentration Measurement | — | — |
| 0x042B | Formaldehyde Concentration Measurement | — | — |
| 0x042C | PM1 Concentration Measurement | — | — |
| 0x042D | PM10 Concentration Measurement | — | — |
| 0x042E | Total Volatile Organic Compounds Concentration Measurement | — | — |
| 0x042F | Radon Concentration Measurement | — | — |
| 0x0430 | Soil Measurement | — | — |
| 0x0431 | Ambient Context Sensing | — | — |
| 0x0432 | Ambient Sensing Union | — | — |
| 0x0433 | Proximity Ranging | — | — |
| 0x0434 | Smoke Concentration Measurement | — | — |
| 0x0450 | Network Identity Management | — | — |
| 0x0451 | Wi-Fi Network Management | — | — |
| 0x0452 | Thread Border Router Management | — | — |
| 0x0453 | Thread Network Directory | — | — |
| 0x0455 | Commissioning Proxy | — | — |
| 0x0503 | Wake On LAN | — | — |
| 0x0504 | Channel | — | — |
| 0x0505 | Target Navigator | — | — |
| 0x0506 | Media Playback | — | — |
| 0x0507 | Media Input | — | — |
| 0x0508 | Low Power | — | — |
| 0x0509 | Keypad Input | — | — |
| 0x050A | Content Launcher | — | — |
| 0x050B | Audio Output | — | — |
| 0x050C | Application Launcher | — | — |
| 0x050D | Application Basic | — | — |
| 0x050E | Account Login | — | — |
| 0x050F | Content Control | — | — |
| 0x0510 | Content App Observer | — | — |
| 0x0511 | Media File Management | — | — |
| 0x0512 | Audio Control | — | — |
| 0x0550 | Zone Management | — | — |
| 0x0551 | Camera AV Stream Management | — | — |
| 0x0552 | Camera AV Settings User Level Management | — | — |
| 0x0553 | WebRTC Transport Provider | — | — |
| 0x0554 | WebRTC Transport Requestor | — | — |
| 0x0555 | Push AV Stream Transport | — | — |
| 0x0556 | Chime | — | — |
| 0x0557 | AV Analysis | — | — |
| 0x0700 | Commodity Tariff | — | — |
| 0x0750 | Ecosystem Information | — | — |
| 0x0751 | Commissioner Control | — | — |
| 0x0752 | Joint Fabric Datastore | — | — |
| 0x0753 | Joint Fabric Administrator | — | — |
| 0x0801 | TLS Certificate Management | — | — |
| 0x0802 | TLS Client Management | — | — |
| 0x0B06 | Meter Identification | — | — |
| 0x0B07 | Commodity Metering | — | — |

## 4. Zigbee ZHA 设备（带支持标记）

| Device ID | Name | MiniHub | 类型 |
|---|---|---|---|
| 0x0000 | Generic On/Off Switch | ✅ yes | switch |
| 0x0001 | Level Control Switch | ✅ yes | switch |
| 0x0002 | On/Off Output | ❌ no |  |
| 0x0003 | Level Controllable Output | ❌ no |  |
| 0x0004 | Scene Selector | ❌ no |  |
| 0x0005 | Configuration Tool | ❌ no |  |
| 0x0006 | Remote Control | ❌ no |  |
| 0x0007 | Combined Interface | ❌ no |  |
| 0x0008 | Range Extender | ❌ no |  |
| 0x0009 | Mains Power Outlet | ✅ yes | light |
| 0x000A | Door Lock | ❌ no |  |
| 0x000B | Door Lock Controller | ❌ no |  |
| 0x000C | Simple Sensor | ✅ yes | sensor |
| 0x000D | Consumption Awareness | ❌ no |  |
| 0x0050 | Home Gateway | ❌ no |  |
| 0x0051 | Smart plug | 🟡 param | power |
| 0x0052 | White Goods | ❌ no |  |
| 0x0053 | Meter Interface | ❌ no |  |
| 0x0100 | On/off light | ✅ yes | light |
| 0x0101 | Dimmable light | ✅ yes | light |
| 0x0102 | Color dimmable light | ✅ yes | light |
| 0x0103 | On/off light switch | ✅ yes | switch |
| 0x0104 | Dimmer switch | ✅ yes | switch |
| 0x0105 | Color dimmer switch | ✅ yes | switch |
| 0x0106 | Light sensor | ✅ yes | sensor_illuminance |
| 0x0107 | Occupancy sensor | ✅ yes | sensor_occupancy |
| 0x0108 | On/off ballast | ✅ yes | light |
| 0x0109 | Dimmable ballast | ✅ yes | light |
| 0x010A | On/off plug-in unit | ✅ yes | light |
| 0x010B | Dimmable plug-in unit | ✅ yes | light |
| 0x010C | Color temperature light | ✅ yes | light |
| 0x010D | Extended color light | ✅ yes | light |

## 5. BLE Mesh SIG 模型（带支持标记）

| Model ID | Name | Group | MiniHub |
|---|---|---|---|
| 0x0000 | Config Server | Foundation | ❌ no |
| 0x0001 | Config Client | Foundation | ❌ no |
| 0x0002 | Health Server | Foundation | ❌ no |
| 0x0003 | Health Client | Foundation | ❌ no |
| 0x0004 | Remote Provisioning Server | Foundation | ❌ no |
| 0x0005 | Remote Provisioning Client | Foundation | ❌ no |
| 0x000A | Mesh Private Beacon Server | Foundation | ❌ no |
| 0x000B | Mesh Private Beacon Client | Foundation | ❌ no |
| 0x1000 | Generic OnOff Server | Generic | ❌ no |
| 0x1001 | Generic OnOff Client | Generic | ❌ no |
| 0x1002 | Generic Level Server | Generic | ❌ no |
| 0x1003 | Generic Level Client | Generic | ❌ no |
| 0x1004 | Generic Default Transition Time Server | Generic | ❌ no |
| 0x1005 | Generic Default Transition Time Client | Generic | ❌ no |
| 0x1006 | Generic Power OnOff Server | Generic | ❌ no |
| 0x1007 | Generic Power OnOff Setup Server | Generic | ❌ no |
| 0x1009 | Generic Power Level Server | Generic | ❌ no |
| 0x100B | Generic Battery Server | Generic | ❌ no |
| 0x100D | Generic Location Server | Generic | ❌ no |
| 0x1011 | Generic User Property Server | Generic | ❌ no |
| 0x1012 | Generic Admin Property Server | Generic | ❌ no |
| 0x1013 | Generic Manufacturer Property Server | Generic | ❌ no |
| 0x1014 | Generic Client Property Server | Generic | ❌ no |
| 0x1100 | Sensor Server | Sensor | ❌ no |
| 0x1101 | Sensor Client | Sensor | ❌ no |
| 0x1102 | Sensor Setup Server | Sensor | ❌ no |
| 0x1200 | Time Server | Time | ❌ no |
| 0x1201 | Time Client | Time | ❌ no |
| 0x1203 | Scene Server | Time | ❌ no |
| 0x1204 | Scene Client | Time | ❌ no |
| 0x1206 | Scheduler Server | Time | ❌ no |
| 0x1207 | Scheduler Client | Time | ❌ no |
| 0x1300 | Light Lightness Server | Light | ❌ no |
| 0x1301 | Light Lightness Client | Light | ❌ no |
| 0x1302 | Light Lightness Setup Server | Light | ❌ no |
| 0x1303 | Light CTL Server | Light | ❌ no |
| 0x1304 | Light CTL Client | Light | ❌ no |
| 0x1305 | Light CTL Setup Server | Light | ❌ no |
| 0x1306 | Light CTL Temperature Server | Light | ❌ no |
| 0x1307 | Light HSL Server | Light | ❌ no |
| 0x1308 | Light HSL Client | Light | ❌ no |
| 0x1309 | Light HSL Setup Server | Light | ❌ no |
| 0x130A | Light HSL Hue Server | Light | ❌ no |
| 0x130B | Light xyL Server | Light | ❌ no |
| 0x130C | Light xyL Client | Light | ❌ no |
| 0x130D | Light LC Server | Light | ❌ no |
| 0x130E | Light LC Client | Light | ❌ no |
| 0x130F | Light LC Setup Server | Light | ❌ no |

## 6. 参数对应（来自三张能力图，MiniHub 已落地部分）

| MiniHub 参数 | Matter | Zigbee | BLE Mesh | spec 集群 |
|---|---|---|---|---|
| `battery_low` |  |  |  |  |
| `brightness` |  |  |  |  |
| `cct` |  |  |  |  |
| `co2` | cl0x040D |  | prop:0x0077 | Carbon Dioxide Concentration Measurement |
| `contact` |  |  |  |  |
| `current_a` |  |  |  |  |
| `energy_wh` |  |  |  |  |
| `hue` |  |  |  |  |
| `humidity` | cl0x0405 |  | prop:0x0076 | Relative Humidity Measurement |
| `illuminance` | cl0x0400 |  | prop:0x0055 | Illuminance Measurement |
| `motion` | cl0x002F |  | prop:0x0042 | Power Source |
| `occupancy` | cl0x0406 |  | prop:0x004d | Occupancy Sensing |
| `power` |  |  |  |  |
| `power_w` |  |  |  |  |
| `pressure_hpa` | cl0x0403 |  | prop:0x00a9 | Pressure Measurement |
| `saturation` |  |  |  |  |
| `tamper` |  |  |  |  |
| `temperature` | cl0x0402 |  | prop:0x0075 | Temperature Measurement |
| `voltage_v` |  |  |  |  |
| `zone_status` |  |  |  |  |

## 7. 已知缺陷（建表时发现，需修正 — 在 Matter `sensors`）

- `motion`(0x002F) 在 matter_map.json 的 `sensors` 中 `property=0x002F` 被当作集群 id 解析，
  但 0x002F 是 *Power Source* 集群，并非运动；ZCL/Matter 无独立 motion 集群，运动经
  OccupancySensing(0x0406) 的 occupancy 位图 bit1 表达。建议 motion 改指 0x0406（与 occupancy
  同集群，由解码层按位区分）或 Matter 下不单独支持 motion。
- `co2`(0x040D) 经核对为**合法** ZCL 集群 *Carbon Dioxide Concentration Measurement*
  （与 BLE 的 CO2 属性 id 0x040D 恰好同号、不同命名空间），故 co2 在 Matter 下可正常解析，非缺陷。
- `property` 字段语义按协议不同：BLE=`BLE Property ID`，Matter=`Matter 集群 id`。

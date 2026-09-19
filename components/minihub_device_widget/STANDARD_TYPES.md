# MiniHub Device Standard Types

[English](STANDARD_TYPES.md) | [中文](STANDARD_TYPES_CN.md) · [Component README](README.md)

RainMaker-aligned layers: **Devices → Services → Parameters → UI Elements**.
Parameter keys drop the `esp.param.` prefix (`power`, not `esp.param.power`).

Machine-readable: [`model/devices.csv`](model/devices.csv),
[`model/services.csv`](model/services.csv),
[`model/parameters.csv`](model/parameters.csv),
[`model/ui_elements.csv`](model/ui_elements.csv).

Upstream: https://docs.rainmaker.espressif.com/docs/dev/firmware/fw_usage_guides/custom-types-standard-types/

## 1. Devices

| Device ID | RainMaker Type | Parameters | Primary | Mandatory |
|-----------|----------------|------------|---------|-----------|
| light | esp.device.lightbulb | power, brightness, cct, hue, saturation, intensity, light-mode | power | power |
| switch | esp.device.switch | power, voltage_v, current_a, power_w, energy_wh | power | power |

`esp.device.light` = `esp.device.lightbulb`. `name` is metadata only (not listed
on these types).

## 2. Services

Reference in `model/services.csv` — not part of the widget registry.

| Service ID | RainMaker Type | Parameters |
|------------|----------------|------------|
| ota | esp.service.ota | ota_url, ota_status, ota_info |
| schedule | esp.service.schedule | schedules |
| time | esp.service.time | tz, tz_posix |
| system | esp.service.system | reboot, factory-reset, wi-fi-reset |

## 3. Parameters

| Parameter | Key | Data Type | Properties | Min, Max, Step | UI Element | Description |
|-----------|-----|-----------|------------|----------------|------------|-------------|
| Name | name | String | Read, Write | 1, 32, – | text | Device name |
| Power | power | Bool | Read, Write | N/A | ui.toggle | On/off |
| Brightness | brightness | Int | Read, Write | 0, 100, 1 | ui.slider | Brightness % |
| Color Temperature | cct | Int | Read, Write | 2700, 6500, 100 | ui.slider | CCT (K) |
| Hue / 颜色 | hue | Int | Read, Write | 0, 360, 1 | ui.slider | Color (deg); label 颜色 |
| Saturation | saturation | Int | Read, Write | 0, 100, 1 | ui.slider | Saturation % |
| Intensity | intensity | Int | Read, Write | 0, 100, 1 | ui.slider | Intensity % |
| Light Mode | light-mode | Int | Read, Write | 0, 2, 1 | ui.dropdown | 0 HSV+CCT, 1 HSV, 2 CCT |
| Voltage | voltage_v | Float | Read | 0, 250, 0.1 | ui.text | Line voltage (V); read-only metering |
| Current | current_a | Float | Read | 0, 16, 0.01 | ui.text | Current (A); read-only metering |
| Active power | power_w | Float | Read | 0, 4000, 1 | ui.text | Power (W); not on/off `power`; read-only |
| Energy | energy_wh | Float | Read | 0, 999999, 1 | ui.text | Cumulative Wh; read-only metering |

| Parameter | Devices |
|-----------|---------|
| name | — (metadata) |
| power | light, switch |
| brightness, cct, hue, saturation, intensity, light-mode | light |
| voltage_v, current_a, power_w, energy_wh | switch |

Icons: device avatars under `images/devices/`; optional control icons under
`images/parameters/`. See [README.md](README.md#icons).

## 4. UI Elements

| UI Control | Web widget | Used for |
|------------|------------|----------|
| ui.toggle | toggle | Bool (`power`) |
| ui.slider | slider | Bounded setpoints; `hue` rainbow via CSS tone |
| ui.dropdown | dropdown | Fixed set (`light-mode`) |
| ui.text | text | String (`name`) or metering readouts; read-only when `access:R` |
| ui.hidden | hidden | Type-level never render (≠ instance `visible:false`) |

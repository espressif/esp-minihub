# MiniHub 设备标准类型

[English](STANDARD_TYPES.md) | [中文](STANDARD_TYPES_CN.md) · [组件说明](README_CN.md)

对齐 RainMaker 四层：**设备 → 服务 → 参数 → UI 元素**。参数 key 去掉
`esp.param.` 前缀（写 `power`，不写 `esp.param.power`）。

机器可读：[`model/devices.csv`](model/devices.csv)、
[`model/services.csv`](model/services.csv)、
[`model/parameters.csv`](model/parameters.csv)、
[`model/ui_elements.csv`](model/ui_elements.csv)。

上游：https://docs.rainmaker.espressif.com/docs/dev/firmware/fw_usage_guides/custom-types-standard-types/

## 1. 设备

| 设备 ID | RainMaker 类型 | 参数 | 主参数 | 必选 |
|---------|----------------|------|--------|------|
| light | esp.device.lightbulb | power, brightness, cct, hue, saturation, intensity, light-mode | power | power |
| switch | esp.device.switch | power, voltage_v, current_a, power_w, energy_wh | power | power |

`esp.device.light` = `esp.device.lightbulb`。`name` 仅作元数据（不在上表类型列表中）。

## 2. 服务

参考数据，见 `model/services.csv` —— 不参与控件注册表。

| 服务 ID | RainMaker 类型 | 参数 |
|---------|----------------|------|
| ota | esp.service.ota | ota_url, ota_status, ota_info |
| schedule | esp.service.schedule | schedules |
| time | esp.service.time | tz, tz_posix |
| system | esp.service.system | reboot, factory-reset, wi-fi-reset |

## 3. 参数

| 参数 | Key | 数据类型 | 属性 | 最小, 最大, 步长 | UI 元素 | 说明 |
|------|-----|----------|------|------------------|---------|------|
| 名称 | name | String | Read, Write | 1, 32, – | text | 设备名称 |
| 电源 | power | Bool | Read, Write | N/A | ui.toggle | 开关 |
| 亮度 | brightness | Int | Read, Write | 0, 100, 1 | ui.slider | 亮度 % |
| 色温 | cct | Int | Read, Write | 2700, 6500, 100 | ui.slider | 相关色温（K） |
| 颜色 | hue | Int | Read, Write | 0, 360, 1 | ui.slider | 颜色（度；id 仍为 `hue`） |
| 饱和度 | saturation | Int | Read, Write | 0, 100, 1 | ui.slider | 饱和度 % |
| 强度 | intensity | Int | Read, Write | 0, 100, 1 | ui.slider | 强度 % |
| 灯光模式 | light-mode | Int | Read, Write | 0, 2, 1 | ui.dropdown | 0 HSV+CCT，1 HSV，2 CCT |
| 电压 | voltage_v | Float | Read | 0, 250, 0.1 | ui.text | 电压（V）；只读计量 |
| 电流 | current_a | Float | Read | 0, 16, 0.01 | ui.text | 电流（A）；只读计量 |
| 功率 | power_w | Float | Read | 0, 4000, 1 | ui.text | 有功功率（W）；不是开关 `power`；只读 |
| 电量 | energy_wh | Float | Read | 0, 999999, 1 | ui.text | 累计电量（Wh）；只读计量 |

| 参数 | 设备 |
|------|------|
| name | —（元数据） |
| power | light, switch |
| brightness, cct, hue, saturation, intensity, light-mode | light |
| voltage_v, current_a, power_w, energy_wh | switch |

图标：设备头像在 `images/devices/`；可选控件图标在 `images/parameters/`。见
[README_CN.md](README_CN.md#icons)。

## 4. UI 元素

| UI 控件 | Web 控件 | 用途 |
|---------|----------|------|
| ui.toggle | toggle | Bool（`power`） |
| ui.slider | slider | 有界设定值；`hue` 彩虹由 CSS tone |
| ui.dropdown | dropdown | 固定选项（`light-mode`） |
| ui.text | text | String（`name`）或计量读数；`access:R` 时只读 |
| ui.hidden | hidden | 类型级不渲染（≠ 实例 `visible:false`） |

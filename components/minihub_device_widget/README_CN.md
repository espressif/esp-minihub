# minihub_device_widget（设备控件描述符）

[English](README.md) | [中文](README_CN.md) · [标准类型](STANDARD_TYPES_CN.md)

设备 UI 类型目录：某类型暴露哪些参数，以及每个参数如何渲染（控件、标签、范围、
CSS）。数据源：`model/*.csv`。构建时嵌入扁平注册表；图标打进 littlefs
`/data/images`。

**规则**

- 目录按**参数名**索引（无 per-device 覆盖列，无 `visible` 列）。
- **一设备一点**（`<protocol>.<device>`）；实时值 / access / 可见性 / 实例上下限在
  `props.params.<name>`。
- 目录样式 = 默认；实例 `devices.json` 有同名字段时覆盖。
- `ui_element` **封闭集**：`ui.toggle` / `ui.slider` / `ui.dropdown` / `ui.text` /
  `ui.hidden`。
- 传输前缀来自设备注册，不在本目录（协议无关）。

## `model/*.csv`

### `devices.csv`

| 列 | 内容 |
|----|------|
| `device_id` | 类型 id，如 `light`、`switch` |
| `parameters` | 逗号分隔的参数名 |
| `primary_param` | 如 `power` |
| `mandatory_params` | 逗号分隔的必选参数 |
| `image` | 可选设备图标文件名（在 `images/devices/` 下） |

### `parameters.csv`

| 列 | 内容 |
|----|------|
| `parameter` | key，不带 `esp.param.` 前缀 |
| `label_zh` | 中文显示名——默认语言，也是回落语言 |
| `label_en` | 英文显示名——留空则回落到 `label_zh` |
| `data_type` | `Bool` / `Int` / `String` / … |
| `properties` | 如 `Read/Write` |
| `min` / `max` / `step` / `unit` | 滑杆默认范围 |
| `ui_element` | 上述封闭集 |
| `options` | `ui.dropdown` 的 JSON 数组；每项用 `labels: {"zh":…, "en":…}`，不再写平铺的 `label` |
| `description` | 自由文本 |
| `css` | 渲染 CSS |
| `image` | 可选控件图标文件名（在 `images/parameters/` 下） |
| `requires` | 可选前提。默认一个参数名（如 `power`，等同 `{"all":["power"]}`）。JSON：`{"all":[...]}`（相与）、`{"any":[...]}`（相或）、`{"not":[...]}`（列出的参数有一个开着就禁用）。`not` 可与 `all` 或 `any` 同写，二者不能一起出现。留空表示没有前提。对象在 CSV 里要加引号。 |

参数名重复，或设备列出目录中不存在的名字 → 生成器报错。

### `ui_elements.csv`

记录封闭的 `ui.*` id（映射为 Web 的 `toggle` / `slider` / `dropdown` / `text` /
`hidden`）。`hue` / `saturation` / `cct` 均为 `ui.slider`；彩虹/色带由参数名 + CSS
决定，不是额外控件类型。

| | `ui.hidden` | 实例 `visible:false` |
|--|-------------|----------------------|
| 写在哪 | `parameters.csv` 的 `ui_element` | 插件 `devices.json` |
| 含义 | 类型级永不画控件 | 本台设备隐藏该参数（仍在 `props.params`） |

只读传感器：实例 `access:"R"`（仍显示，无拖动控件）。

### `services.csv`

仅参考（RainMaker 系统服务），不参与控件注册表。

### 内置类型

- **`light`** — `power`、`brightness`、`cct`、`hue`（标签「颜色」）、`saturation`、
  `intensity`
- **`switch`** — `power`、`voltage_v`、`current_a`、`power_w`、`energy_wh`
- **`speaker`** — `power`、`volume`（`volume` 依赖 `power`）
- **`name`** — 在目录中；无设备列出 → 不成控件

完整表见 [`STANDARD_TYPES_CN.md`](STANDARD_TYPES_CN.md)。

<a id="icons"></a>
## 图标

不编译进固件。构建把 `images/` 打进 `/data/images`。

| 目录 | 作用 | 约定 |
|------|------|------|
| `images/devices/` | 设备头像 | `<device>.svg` |
| `images/parameters/` | 可选控件图标 | `<device>_<parameter>.svg` 或 `<parameter>.svg` |

交互控件是 `minihub_web` 里的 CSS 组件（`device_widgets.js` / `.css`）。解析：
`minihub_device_widget_image_name(device, parameter, …)` → 相对 `/data/images` 的路径。
经 `GET /img/<subdir>/<name>` 提供；`/api/device_widgets` 也会带上 URL。

## 生成器

```bash
python3 tools/gen_device_widget.py --check
python3 tools/gen_device_widget.py --out-dir /tmp/dw_out
python3 tools/gen_svgs.py          # 可选：重生成设备 SVG；--png 需 cairosvg
```

CMake 只写构建目录产物（勿提交）：扁平 `devices.csv`、`ui_controls.json`、
`minihub_device_widget_gen.c`。脚本仅用标准库。

<a id="remote-catalog"></a>
## 远程更新类型目录（不重烧）

| 方式 | 做法 |
|------|------|
| 固件嵌入 | 改 `model/*.csv` → 重建 → 烧录 |
| 远程更新 | `PUT` 模型 `devices.csv` + `parameters.csv`（需 `CONFIG_MINIHUB_WEB_UI`，鉴权同 REST） |

| 文件 | 设备路径 |
|------|----------|
| 上传的模型 CSV | `/data/config/model/devices.csv`、`parameters.csv` |
| 扁平运行时注册表 | `/data/config/devices.csv` |

两个模型文件都齐时，固件按与 `gen_device_widget.py` 相同规则扁平化，写入运行时
CSV，并 `minihub_device_widget_reload()`——无需重启。校验失败会拒绝。远程只改类型；
真实设备仍需插件注册 point。

```bash
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  --data-binary @components/minihub_device_widget/model/devices.csv \
  http://$HUB/api/dev/catalog/devices.csv
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  --data-binary @components/minihub_device_widget/model/parameters.csv \
  http://$HUB/api/dev/catalog/parameters.csv
```

本地面板：

```bash
python3 tools/dev_sync.py --open
```

见 [`tools/README_CN.md`](../../tools/README_CN.md)。

## C API

```c
#include "minihub_device_widget.h"

const minihub_device_widget_device_t *d = minihub_device_widget_get("light");
const minihub_device_widget_entry_t *e =
    minihub_device_widget_find("light", "brightness");

char icon[MINIHUB_DW_IMAGE_PATH_MAX];
minihub_device_widget_image_name("light", "brightness", icon, sizeof(icon));
minihub_device_widget_image_name("light", NULL, icon, sizeof(icon));  // 设备头像

char path[MINIHUB_POINT_ID_MAX];
minihub_point_resolve_by_type_param("light", "power", path, sizeof(path));
// -> 设备 point id（如 "sim.color_bulb"）；参数在 props.params
```

## 修改 / 新增类型

1. 改 `model/devices.csv`（参数列表）与 `parameters.csv`（仅补新名字）。
2. 可选：`images/devices/<device>.svg`。
3. `python3 tools/gen_device_widget.py --check`，再重建 Host。
4. 只提交 `model/*` 与 `images/*`。

## 新增 UI 控件

封闭集很小。确需新增时：

1. `model/ui_elements.csv` + 生成器 `CLOSED_UI` / `UI_WIDGET`
2. 在 [`../minihub_web/web/js/device_widgets.js`](../minihub_web/web/js/device_widgets.js) 渲染

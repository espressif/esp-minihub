# 示例插件：color_bulb（彩色球泡灯，在线模拟）

[English](README.md) | [中文](README_CN.md)

位于 `examples/plugins/` 的独立 L3 native 演示（**不**在默认 `application/minihub`）。注册**两盏在线彩色球泡灯**（`type=light`）并响应
Web UI —— **无真实硬件**。

使用 Host 的 `light` 类型目录（`minihub_device_widget/model` → 运行时
`/data/config/devices.csv` + `/data/images`）。**不**把 label / widget / CSS / 图标
打进插件包。

**一设备一点：** `sim.kitchen_lamp` 与 `sim.living_lamp`，实时值分别在各自
`props.params`。

## 文件

| 文件 | 作用 |
|------|------|
| `module.json` | 清单（`id=color_bulb`，runtime `native`） |
| `module_entry.c` | `minihub_module_entry`：适配器 + Host API 表 |
| `color_bulb_adapter.c` | `start()` 为每个 `sim.<device>` 登记点；命令按 `point_id` 路由 |
| `devices.json` | 实例数组：`device` / `type` / `params[]`；省略的上下限用目录默认。存在时打进 `.mhub` 与工厂预装 |
| `CMakeLists.txt` | 经 `MINIHUB_PLUGIN_DIRS` 拉入的 IDF 组件；打成 MHMD `.so` |
| `device_desc.gen.h` | 由 `devices.json` 生成；构建前须存在，**勿手改**（嵌入回退） |

## 构建

### 独立编译（无需 IDF）

```bash
python3 tools/build_plugin_standalone.py color_bulb
# 可选：--pack（examples 需设置 MINIHUB_PLUGIN_DIRS）
```

MHMD 输出：`application/minihub/build/color_bulb.so`。CMake 会根据
`devices.json` 重新生成 `device_desc.gen.h`。

### 随 Host 树内编译

先生成头文件（如需），再用环境变量把本目录拉进 Host configure：

```bash
python3 tools/generate_devices_property_from_json.py \
    examples/plugins/color_bulb/devices.json \
    examples/plugins/color_bulb/device_desc.gen.h

export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # 或 .../examples/plugins/color_bulb
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` 在 **configure** 期读取
（`cmake/minihub_bundle/minihub_import.cmake`）。不会自动工厂预装。

需 Dynamic Store Modules（`CONFIG_MINIHUB_DYNAMIC_MODULES`；默认产品
`dynamic_modules` 已开）。

### 安装 — 工厂预装（可选）

```yaml
# application/minihub/manifest.yml — 临时
store_preinstall: [color_bulb]
```

保持 `MINIHUB_PLUGIN_DIRS` 后重新编译烧录；演示完请清空该列表。

### 安装 — Store（不改 manifest）

仅设 `MINIHUB_PLUGIN_DIRS` 时仍会编 `libadapter_color_bulb.so`，并可暂存
`color_bulb.mhub`。从 Store UI 安装，或：

```bash
python3 tools/pack_store_module.py pack color_bulb
python3 tools/sync_store_catalog.py all
```

Store 安装 / `module_attach` 会写入 `adapters.json` 的 `color_bulb` 槽
（`ensure_adapter_slot`）。无该槽时 `load_all` 会跳过适配器。

重编 Host（控制路径 + Web UI）后，还需重烧 / 重新部署 web 资源
（`modstore-flash` 或完整 `flash`），设备页才会为每张卡带上 `point_id`。

## 改参数

编辑 `devices.json` → 重新生成 `device_desc.gen.h` → 再编译。参数名须与 `light`
目录一致（`power`、`brightness`、`hue`、`saturation`、`cct` 等）。

## 运行结果

`start()` 按以下顺序加载实例描述：

1. Host `module_read_file(adapter, "devices.json")` — 读 Store 安装目录下与
   `module.json` / `.so` 同级的文件（不硬编码路径）
2. 若 API 不可用、文件缺失或无 Store 安装目录的静态链接，则回退到
   `device_desc.gen.h` 中的嵌入式 `DEVICE_DESC_JSON`

打包（`tools/pack_store_module.py`）与工厂预装（modstore CMake）在源码树存在
`devices.json` 时会一并打入。

- Overview：两张在线 `light` 卡（`sim.kitchen_lamp`、`sim.living_lamp`）
- Devices：厨房灯有电源 + 亮度/色相/饱和度（`cct` 隐藏）；客厅灯有电源 + 亮度
- 控制：`POST /api/devices/control` 带 `point_id` → 适配器更新对应实例的
  `props.params`

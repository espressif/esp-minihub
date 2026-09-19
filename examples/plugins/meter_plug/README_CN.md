# 示例插件：meter_plug（计量插座，在线模拟）

[English](README.md) | [中文](README_CN.md)

位于 `examples/plugins/` 的独立 L3 native 演示（**不**在默认 `application/minihub`
或 `components/minihub_plugins/`）。注册**一个在线计量插座**（`type=switch`：开关 +
电压/电流/功率）—— **无真实硬件**。

使用 Host 的 `switch` 类型目录。**不**拷贝 label / widget / CSS / 图标。

**一设备一点：** `sim.meter_plug`，值在 `props.params`。

## 文件

| 文件 | 作用 |
|------|------|
| `module.json` | 清单（`id=meter_plug`，runtime `native`） |
| `module_entry.c` | `minihub_module_entry`：适配器 + Host API 表 |
| `meter_plug_adapter.c` | 注册 `sim.meter_plug` + 开关/计量模拟 |
| `devices.json` | 实例：`type=switch`，参数 `power` / `voltage_v` / `current_a` / `power_w` |
| `device_desc.gen.h` | 由 `devices.json` 生成；**勿手改** |
| `CMakeLists.txt` | 经 `MINIHUB_PLUGIN_DIRS` 拉入；打成 MHMD `.so` |

## 行为

- `point_id` = `sim.meter_plug`，`type` = `switch`，`adapter` = `meter_plug`
- `power` 可读写；计量参数 `access:"R"`（UI 只读）
- 开：电压 ≈ 220 V，电流 ≈ 0.45 A，功率 ≈ V×I
- 关：电流/功率 ≈ 0，线电压仍 ≈ 220 V
- 命令：`turn_on` / `turn_off` / `toggle` / `set`（Host `point_set`）
- 需要 `adapters.json` 的 `meter_plug` 槽（Store 安装 / `module_attach` 时
  `ensure_adapter_slot`）

## 构建

```bash
python3 tools/generate_devices_property_from_json.py \
    examples/plugins/meter_plug/devices.json \
    examples/plugins/meter_plug/device_desc.gen.h

export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # 或 .../examples/plugins/meter_plug
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` 在 configure 期读取（`cmake/minihub_bundle/minihub_import.cmake`）。

### 工厂预装（可选）

```yaml
# application/minihub/manifest.yml — 临时
store_preinstall: [meter_plug]
```

保持 `MINIHUB_PLUGIN_DIRS` 后重新编译烧录；演示完请清空该列表。

### Store 安装（不改 manifest）

```bash
python3 tools/pack_store_module.py pack meter_plug
python3 tools/sync_store_catalog.py all
```

烧录并安装后，Overview / Devices 出现一张 switch 卡（`sim.meter_plug`）。

# MiniHub 示例

[English](README.md) | [中文](README_CN.md)

## plugins/（示例插件）

| 目录 | 层级 | 目的 |
|------|------|------|
| [ble_mesh_pack](plugins/ble_mesh_pack/) | **L1** | 把额外 BLE Mesh `provision.bind_models` 合并进 Host `adapters.ble_mesh`。不登记点、无 JS、无 `.so`。 |
| [color_bulb_js](plugins/color_bulb_js/) | **L2** | 三盏模拟 `type=light`（`sim.js_kitchen`、`sim.js_desk`、`sim.js_bedroom`）+ JS 命令处理。设备页控件来自 Host 类型目录。 |
| [meter_plug_js](plugins/meter_plug_js/) | **L2** | 一台模拟计量插座（`sim.plug`：开关 + 电压/电流/功率，计量只读）。 |

仓库内不再有 L3 示例：原生 `.so` 必须针对某个 Host ABI 编译，指南改用
`tools/new_store_module.py --tier l3` 脚手架（见 `docs/zh_CN/plugins/develop_l3.rst`）。

厂商 Store 插件不在本仓库。在 configure / pack 之前把 `MINIHUB_PLUGIN_DIRS` 指到旁路插件仓库。

射频 OTA 插件 **不得** 再 `point_register` 第二个 id。用 `merge_props` 丰富
Host 已有点（`owner` / `ota`）。

## 打包（开发机）

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack <id>    # 或 pack all（默认跳过原生）
python3 tools/sync_store_catalog.py
python3 tools/store_server.py --port 8080
```

Hub：Store `custom_url` → 刷新 → 安装。REST 的 `type`：L1 为 `pack`，L2/L3 为
`module`。然后 `POST /api/hub/reload`。

L3 还需要对应 `.so`。仓库内编译：在 `./tools/build_firmware.py modules`
**之前** 导出 `MINIHUB_PLUGIN_DIRS`。Host 的 `build` 不编译原生插件。不编 IDF 工程（需要 `riscv32-esp-elf-gcc`
和 `cmake`；MHMD 文件在 `plugins/<id>/build/<id>.so`）：

```bash
python3 tools/new_store_module.py --tier l3 --id my_native
python3 tools/build_plugin_standalone.py my_native
```

然后再按上面打包。L1/L2 只打 zip，不必编固件。L3 需要 `.so`
（`build_plugin_standalone.py` 或 `./tools/build_firmware.py modules`）。

新建模块脚手架：`python3 tools/new_store_module.py --tier l1|l2|l3 --id …`
(默认写到 `examples/plugins/`；厂商仓库用 `--out` / `MINIHUB_PLUGIN_DIRS`)。

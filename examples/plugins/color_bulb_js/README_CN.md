# 示例插件：color_bulb_js（L2 JS 彩灯）

[English](README.md) | [中文](README_CN.md)

独立 **L2 QuickJS** 演示，位于 `examples/plugins/`（不在默认
`application/minihub`）。注册 **三盏在线模拟灯**
（`type=light`），带设备页控件与 JS 命令处理 —— **无真实硬件**。

与 [`meter_plug_js`](../meter_plug_js/)（同一 JS 思路的插座版）配套，替代已删除的
`color_bulb` L3 C 演示。

## 何时使用

| 需求 | 选择 |
|------|------|
| 原生 `.so` + 嵌入 `devices.json` | **L3** 原生插件 |
| 用 QuickJS 做同样体验（原型/教学） | **L2** 本演示 |
| 驱动已有 Zigbee/L3 点位 | **L2** `auto_off` |

需要 Host 固件包含 `components/minihub_js/minihub_js.c` 中的扩展绑定
（`register` 支持 `params`/`props`、`point.set` 可选 `props`、`host.command.on`）。
旧固件仍可安装模块，但控制会 404，直到刷入新绑定。

## 运行时

每次 `setup(host)`（开机与 `POST /api/hub/reload`）：

1. `host.command.on("color_bulb_js", handler)` — Core 将
   `adapter=color_bulb_js` 的控制命令路由进 JS。
2. 注册三个带 `params` → Core `props.params` 的点：

| point_id | 可区分初值 |
|----------|------------|
| `sim.js_kitchen` | 关、亮度 80、色相 0 |
| `sim.js_desk` | 开、亮度 40、色相 120 |
| `sim.js_bedroom` | 开、亮度 60、色相 240 |

参数名对齐 Host `light` 类型目录；标签/控件来自 Host device-widget 目录。

## 文件

| 文件 | 作用 |
|------|------|
| `module.json` | 清单（`id=color_bulb_js`，`runtime: js`） |
| `main.js` | `setup(host)` — 三盏灯 + 命令处理 |
| `README.md` / `README_CN.md` | 说明 |

无 `CMakeLists.txt` / `.so`。实例列表嵌在 `main.js`（L2 无 `module_read_file`）。

## 打包与安装

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack color_bulb_js
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

Hub：Store `custom_url` → 刷新 → 安装 `color_bulb_js` →
`POST /api/hub/reload`。

需要 `CONFIG_MINIHUB_DYNAMIC_MODULES` 与 `CONFIG_MINIHUB_JS_PLUGINS`。
公开 Host 的 `store_preinstall` 保持 `[]`。

## 验证

* 串口：`JS module 'color_bulb_js' setup ok` 与三条 `registered sim.js_*`
* `GET /api/points` — 三个 `type=light` 且含 `props.params`
* 设备页：三张灯卡片，开关/滑条可改值
* `POST /api/devices/control` 带 `point_id` 成功（非 404）

## UI 能 / 不能

| | |
|--|--|
| **能** | 显示控件；控制开关/亮度/色相/饱和度；与 L2 兄弟 `meter_plug_js` 共存 |
| **不能** | 定时/事件订阅；运行时加载外部 `devices.json` |

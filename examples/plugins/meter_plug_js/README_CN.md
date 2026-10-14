# 示例插件：meter_plug_js（L2 JS 智能插座）

[English](README.md) | [中文](README_CN.md)

独立 **L2 QuickJS** 演示，位于 `examples/plugins/`（不在默认
`application/minihub`）。注册 **一个在线模拟智能插座**
（`type=switch`），含可写 `power` 参数与只读 `voltage` / `current` / `watts`
遥测，并带 JS 命令处理 —— **无真实硬件**。

这是已删除的 `meter_plug` L3 C 演示的 JS（L2）替代版，与
[`color_bulb_js`](../color_bulb_js/)（同一 JS 思路的灯版）配套。

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

1. `host.command.on("meter_plug_js", handler)` — Core 将
   `adapter=meter_plug_js` 的控制命令路由进 JS。
2. 注册一个带 `params` → Core `props.params` 的点：

| point_id | 参数 |
|----------|------|
| `sim.js_plug` | `power`（RW），`voltage`/`current`/`watts`（RO） |

参数名对齐 Host `switch` 类型目录。`voltage` / `current` / `watts` 声明为
`RO`，拒绝客户端写入。标签/控件来自 Host device-widget 目录。

控制路径与 L3 原生思路一致：更新 `props.params.<name>.value`，再
`host.point.set(id, summary, { params })` 让 UI 刷新。

## 文件

| 文件 | 作用 |
|------|------|
| `module.json` | 清单（`id=meter_plug_js`，`runtime: js`） |
| `main.js` | `setup(host)` — 注册插座 + 命令处理 |
| `README.md` / `README_CN.md` | 说明 |

无 `CMakeLists.txt` / `.so`。实例列表嵌在 `main.js`（L2 无 `module_read_file`）。

## 打包与安装

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack meter_plug_js
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

Hub：Store `custom_url` → 刷新 → 安装 `meter_plug_js` →
`POST /api/hub/reload`。

需要 `CONFIG_MINIHUB_DYNAMIC_MODULES` 与 `CONFIG_MINIHUB_JS_PLUGINS`。
公开 Host 的 `store_preinstall` 保持 `[]`。

## 验证

* 串口：`JS module 'meter_plug_js' setup ok` 与 `registered sim.js_plug`
* `GET /api/points` — `sim.js_plug` 含 `type=switch` 与 `props.params`
* 设备页：一张开关卡片，电源切换可改值
* `POST /api/devices/control` 带 `point_id` 成功（非 404）；写
  `voltage`/`current`/`watts` 返回错误

## UI 能 / 不能

| | |
|--|--|
| **能** | 显示控件；切换电源；与 L2 兄弟 `color_bulb_js` 共存 |
| **不能** | 定时/事件订阅；运行时加载外部 `devices.json`；写只读遥测 |

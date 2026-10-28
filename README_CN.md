# MiniHub

[English](README.md) | [中文](README_CN.md)

面向多协议物联网中枢的 ESP32 Host 固件：Web UI、REST、WebSocket 事件、
Store 模块（`.mhub`），以及 Host 适配器（Zigbee / BLE Mesh / Matter / MQTT /
Modbus / Thread 边界路由器）。

第三方生态以 **Store 插件**（L1 / L2 / L3）交付，不是 Host Core 依赖。
公开 Host 的 `store_preinstall` 保持为空。

**许可证：** [Apache 2.0](LICENSE)

## 适合谁读

| 目标 | 从这里开始 | 不必去做 |
|------|------------|----------|
| 理解架构 | [简介](docs/zh_CN/introduction.rst) · [架构](docs/zh_CN/architecture.rst) | 读 Core `.c` |
| 编译烧录参考 Host | [快速开始](docs/zh_CN/getting_started.rst) | 改 `components/minihub_*` |
| 做 OEM Host 产品 | [Host 定制](docs/zh_CN/host_customization.rst) → [量产](docs/zh_CN/production.rst) | 把 Core 拷进 `application/` |
| 开发 Store 插件（L1/L2/L3） | [插件概览](docs/zh_CN/plugins/overview.rst) · [examples/](examples/README_CN.md) | 改 Host 启动流程 |
| REST / MCP / 规则 | [数据模型](docs/zh_CN/data_model.rst) · [规则](docs/zh_CN/rules.rst) · [API](docs/zh_CN/api/index.rst) | 用 C 写适配器 |
| 射频、认领、OTA | [射频与接入](docs/zh_CN/radios.rst) | 把 Thread 当成设备点 |

编程指南（HTML）：[docs/README.md](docs/README.md)。RST：
[docs/en/index.rst](docs/en/index.rst) · [docs/zh_CN/index.rst](docs/zh_CN/index.rst)。

## 为什么用 MiniHub

* **Host / 插件分离** — Espressif 协议栈与中枢服务留在 Host；厂商云与 OEM 增量走 `.mhub`
* **以点位为中心** — UI、REST、规则、场景用 `point_id`，不用厂商 API
* **OEM 不必 fork Core** — 产品 = `application/APP/`（板级、分区、出厂 JSON）
* **现场升插件** — ABI 匹配时可不刷 Host 安装/升级 Store 模块
* **软重载** — `POST /api/hub/reload` 应用配置与插件，无需整机重启

## 产品

**产品名 = [`application/`](application/) 下的目录名。** 共享 Core 在
[`components/`](components/)。

| 产品 | 路径 | 说明 |
|------|------|------|
| **minihub** | [`application/minihub/`](application/minihub/) | 通用 Host（Web / Store / 适配器）。以太网优先；可选 LCD 见该应用 README。`-app minihub` / `MINIHUB_APP`。 |

见 [`application/README.md`](application/README.md)。

## 环境

* ESP-IDF 6.2，固定在 `master` 的 `434d5fd100b90a03c19018c572809cc29c7db28b`
  （`git describe` = `qa-test-full-v6.2-20261016-70-g434d5fd100b`）；ESP32-S31 的支持比任何
  6.2 release tag 都新。构建读取 `$IDF_PATH`，默认 `./esp-idf`
* Python 3.10+ 与 PyYAML
* 可选：编进 Matter 时设置 `ESP_MATTER_PATH`

## 快速开始

```bash
./tools/build_firmware.py -app minihub build
./tools/build_firmware.py -app minihub -p /dev/ttyUSB0 app-flash monitor
```

协议栈：`zigbee` · `ble_mesh` · `matter` · `thread`（Thread **只做 BR**，不是
设备适配器）。细节见 [快速开始](docs/zh_CN/getting_started.rst)。

## 仓库布局

| 路径 | 作用 |
|------|------|
| `application/` | 按产品划分的固件（目录名即产品名） |
| `components/` | 共享 Core、Host 适配器、Store 运行时 |
| `configs/` | 共享 manifest、适配器种子、sdkconfig 片段 |
| `examples/` | 独立 Store 演示 — 仅一层 [`plugins/`](examples/README_CN.md) |
| `tools/` | 编译、打包、Store 服务、CI |
| `docs/` | 编程指南（RST / esp-docs） |
| `docs/module_sdk/` | L3 Host 导出 allowlist |

## 数据模型（摘要）

Core **按点位工作**，不为灯/开关定义 C 结构体。一台物理设备 = 一条
`minihub_point_t`（`point_id` / `type` 字符串 / 摘要 `value` / `props` /
`adapter` / `online`）。Matter 是 **一个 endpoint 一条点**
（`light/matter_n{node}_ep{ep}`）。Thread 不登记设备点。

* 实时参数在 `props.params.<name>`。身份（`manufacturer`、`category`、
  `owner`、`ota` 等）是 **顶层** `props`，不是类型目录参数。
* UI 控件来自 Host **类型目录**（CSV），与协议无关。
* 自己拥有的设备只 `point_register` 一次。在 Host 射频点上认领 / OTA 必须用
  **同一个** `point_id` 和 `merge_props`，不要登记第二条点。命令仍走
  `point.adapter`（射频）。
* 现场更新用 `merge_params` / `merge_props`，不要整份替换 `props`。
* 规则动作只有 `command` / `command_by_type` / `delay` / `notify`：
  [规则](docs/zh_CN/rules.rst)。场景是独立 REST，不是规则 `then`。

## Store 插件

| 层级 | 包内 | 演示 |
|------|------|------|
| L1 | `module.json` + `inject` | [`ble_mesh_pack`](examples/plugins/ble_mesh_pack/) |
| L2 | `module.json` + `main.js` | [`color_bulb_js`](examples/plugins/color_bulb_js/)、[`meter_plug_js`](examples/plugins/meter_plug_js/) |
| L3 | `module.json` + `.so` | 仓库内无示例 —— 用 `tools/new_store_module.py --tier l3` 生成脚手架 |

索引：[examples/README_CN.md](examples/README_CN.md)。不要把演示写进公开
`store_preinstall`。

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack <id>
```

## 文档

* [English index](docs/en/index.rst) · [中文索引](docs/zh_CN/index.rst)
* [编译 HTML](docs/README.md)
* [贡献](docs/zh_CN/contributing.rst) · [AGENTS.md](AGENTS.md)

## 贡献

见 [docs/zh_CN/contributing.rst](docs/zh_CN/contributing.rst)：约定式提交、
分支名用下划线（不用连字符）、Host Core 保持生态无关、厂商逻辑放 Store 插件。

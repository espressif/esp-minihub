# 示例插件：ble_mesh_pack（L1 配置包）

[English](README.md) | [中文](README_CN.md)

独立 **L1 pack**，位于 `examples/plugins/`（不在默认 `application/minihub`）。包内 **只有** `module.json`：无 `main.js`、无
`.so`、无射频 `devices[]`。

Host `minihub_pack` 在加载或 `POST /api/hub/reload` 时把 `inject` **合并** 进已有
`adapters.ble_mesh`。公开 Host 的 `store_preinstall` 保持 `[]`。

## 何时使用

| 需求 | 选择 |
|------|------|
| 给 **Host** 适配器下默认参数（bind、UUID 过滤、MQTT/Modbus 映射） | **L1** 本演示 |
| 逻辑、HTTP、新适配器 | **L2** / **L3** |
| 对已入网点做厂商认领 / OTA | **L3** 原生模块（同一 `point_id`） |

**不要** 向 `zigbee` / `matter` / `ble_mesh` inject `devices[]`。那些点由入网登记。
MQTT 的 `devices[]` 可以 inject（Host MQTT 列表），但适配器必须先
`enabled`，否则 pack 会被跳过。

## 合并内容

追加一条出厂 `adapters.json` **没有** 的 SIG bind：

| 字段 | 值 |
|------|-----|
| `model_id` | `0x1307`（Light HSL Server） |
| `name` | `l1_demo_light_hsl_srv`（用来辨认合并结果） |

Host 当前 BLE Mesh **客户端** 绑定 OnOff / Level / Lightness / CTL。这一行用于
演示 inject，不是新的调光链路。 **不会** 写入 `uuid_filters`，入网仍接受任意
未配网节点。

若 `adapters.ble_mesh.enabled` 为 **false**，Host **跳过** 本 pack（串口：
`skip pack 'ble_mesh_pack'`）。

## 文件

| 文件 | 作用 |
|------|------|
| `module.json` | 清单（`type`/`runtime`: `pack`）+ `inject` |
| `README.md` / `README_CN.md` | 本说明 |

打包即 zip `module.json`。Store 安装后 Host 拷到
`/data/config/packs/ble_mesh_pack.json`。

## 打包与安装（Store）

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack ble_mesh_pack
python3 tools/sync_store_catalog.py all
python3 tools/store_server.py --port 8080
```

Hub：Store `custom_url` → 刷新 → 安装。REST：

```bash
curl -X POST http://$HUB/api/store/install \
  -H 'Content-Type: application/json' \
  -d '{"type":"pack","id":"ble_mesh_pack"}'
curl -X POST http://$HUB/api/hub/reload
```

## 验证

* 串口：`loaded pack 'BLE Mesh L1 defaults (demo)' (ble_mesh_pack)`，随后
  `merged pack 'ble_mesh_pack' into adapter 'ble_mesh'`
* `GET /api/packs` — `id=ble_mesh_pack`，`inject_adapter=ble_mesh`
* `GET /api/config/adapters` — `bind_models` 含
  `{ "model_id": "0x1307", "name": "l1_demo_light_hsl_srv" }`
* 设备页 **不会** 多一张卡，直到真实 Mesh 节点入网（L1 不登记点）

## UI 能 / 不能

| | |
|--|--|
| **能** | 不重编固件即可改 Host BLE Mesh 配网默认 |
| **不能** | 登记 Mesh 设备、跑 JS、`dlopen`、写 `owner` / OTA |

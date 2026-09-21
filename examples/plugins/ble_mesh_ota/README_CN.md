# 示例插件：ble_mesh_ota（云端目录 + 固件缓存）

[English](README.md) | [中文](README_CN.md)

独立 **L3 native** 演示，位于 `examples/plugins/`（不进公开 Host
`store_preinstall`）。**不注册设备点**。Host BLE Mesh 已为每个节点占一个
`point_id`（`light/bm_%04x`）。本插件：

1. 列出 `adapter=ble_mesh` 的 Host 点（`minihub_point_list_by_adapter`）
2. 打包的 `catalog.json` 只含 Espressif 过滤 + `catalog_url`
3. 厂商对上后在同一点写入 `props.owner=ble_mesh_ota`。总览把卡片从 BLE Mesh 分组挪到厂商分组；`point.adapter` 仍是 `ble_mesh`，开关命令还走 Host
4. **GET `catalog_url`** 拿到要提供的 `version` 和 `firmware_url`（不写进插件）
5. 可选地把固件流式写入 `/data/ble_mesh_ota/<uuid>.bin`
6. 在**同一个** `point_id` 上 merge `props.ota`

Devices 在 `props.ota.available` 为 true 时显示 OTA 按钮。
`POST /api/devices/ota` 仍走 BLE Mesh adapter；**Mesh Firmware Update / BLOB
空中传输仍是 Host 桩**（`ESP_ERR_NOT_SUPPORTED`）。演示到 LittleFS 文件 + UI
标记为止。

L2 JS 做不到：`host.http.request` 正文上限 16 KiB，且无文件 API。

## 文件

| 文件 | 作用 |
|------|------|
| `module.json` | 清单（`id=ble_mesh_ota`，runtime `native`） |
| `module_entry.c` | `minihub_module_entry` |
| `ble_mesh_ota_adapter.c` | 扫描 / 下载 / `point_merge_props` |
| `catalog.json` | 本地：`catalog_url` + `manufacturer`（不含镜像版本） |
| `ui.json` | 总览分组名（`filter_label`：Espressif） |
| `CMakeLists.txt` | 经 `MINIHUB_PLUGIN_DIRS` 拉入的 IDF 组件 |

## 编译

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"   # 或 .../examples/plugins/ble_mesh_ota
./tools/build_firmware.py -app minihub build
```

`MINIHUB_PLUGIN_DIRS` 在 **configure** 期读取，不会自动预装。
需要 `CONFIG_MINIHUB_DYNAMIC_MODULES`。

不要把 `ble_mesh_ota` 写进公开 Host 的 `store_preinstall`。

### 安装 — Store

```bash
python3 tools/pack_store_module.py pack ble_mesh_ota
python3 tools/sync_store_catalog.py all
```

Store 安装会写入 `adapters.json` 的 `ble_mesh_ota` 槽。默认用打包
`catalog.json` 里的 `catalog_url`，`adapters.json` 可覆盖。

云端 `ota-catalog.json`（插件 GET 的内容）：

```json
{
  "devices": [
    {
      "manufacturer": "Espressif",
      "version": "1.0.0",
      "firmware_url": "http://192.168.1.100:8080/1.0.0.bin"
    }
  ]
}
```

`version` / `firmware_url` **只来自这次响应**（`props.ota.version` 是要提供的
镜像，不是 `props.sw_version`）。云端目录拉不到就不标 OTA。
匹配 Composition CID 厂商名（`0x02E5` → `Espressif`），大小写不敏感；尚未读到
厂商名的点会跳过。

RPC `POST /api/modules/ble_mesh_ota/rpc`：`refresh`、`download`。

## 用到的 Host 导出

`mkdir` / `stat` / `unlink` / `rmdir`，流式 `esp_http_client_open` + `read`，
`minihub_point_list_by_adapter`，`minihub_point_merge_props`。插件**不能**调
BLE Mesh 射频 API。

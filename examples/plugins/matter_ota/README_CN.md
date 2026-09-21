# 示例插件：matter_ota（云端目录 + 固件缓存）

[English](README.md) | [中文](README_CN.md)

独立 **L3 native** 演示，位于 `examples/plugins/`（不进公开 Host
`store_preinstall`）。**不注册设备点**。Host Matter 已为每个配网 endpoint
占一个 `point_id`（`light/matter_n*_ep*`）。本插件：

1. 列出 `adapter=matter` 的 Host 点
2. 打包的 `catalog.json` 只含 Espressif 过滤 + `catalog_url`
3. 厂商对上后写入 `props.owner=matter_ota`；`point.adapter` 仍是 `matter`
4. GET `catalog_url` 拿到 `version` / `firmware_url`
5. 可选缓存到 `/data/matter_ota/`
6. 在同一 `point_id` 上 merge `props.ota`

`POST /api/devices/ota` 走 Host CSA 路径（Provider `0x0029`、
`AnnounceOTAProvider`、BDX）。缓存文件须是 Matter OTA 镜像（带 CSA 头），
不是裸的 ESP 固件 `.bin`。Matter-over-Thread 终端也走 Matter 点，不是
`adapter=thread`。

```bash
export MINIHUB_PLUGIN_DIRS="$PWD/examples/plugins"
python3 tools/pack_store_module.py pack matter_ota
```

不要把 `matter_ota` 写进公开 Host 的 `store_preinstall`。

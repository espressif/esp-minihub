# 插件 / 账号绑定（claw_minihub）

claw 固件**不内置**任何厂商云插件。控制与登录由**已安装**的 Store 模块提供（可装 0 个、1 个或多个）。

**相关：** [操作文档索引](README.md) · [`docs/plugins_and_store.md`](../../../docs/plugins_and_store.md) · [冒烟](Plugin-Smoke-Test.md)

---

## 1) 设备连通

1. 设备已上电并联网。
2. （可选）串口 `wechat --qr-start` → 扫码，确认 IM 可用。

---

## 2) 安装插件

```bash
# 仓库根目录 — 以实际需要的插件 id 为准（示例：miot）
python3 tools/pack_store_module.py pack <id>
python3 tools/sync_store_catalog.py all
```

设备上：Store UI 安装，或：

```json
{"method":"POST","path":"/api/store/install","body":"{\"kind\":\"module\",\"id\":\"<id>\",\"reload\":true}"}
```

---

## 3) 串口（通用）

```text
module --list
module --rpc --id <id> --method status
module --rpc --id <id> --method login --params {"username":"...","password":"..."}
```

方法名与参数以该插件的 RPC / `ui.json` 为准。Web：`http://<IP>/minihub`。Agent：`hub.api` → `POST /api/modules/<id>/rpc`。

---

## 4) 不要用的路径

| 路径 | 说明 |
|------|------|
| 固件内 `cmd_miot` / `/account bind miot` | 已移除 |
| 假定必须安装某个插件才能用 claw | 错误 — 无插件时仍可 Zigbee MVP / Agent |
| IM `/miot …` 路由 | 已删除 |

微信门户 / QR 仍走 `http_server` + `cmd_wechat_qr`，与云插件无关。

# 插件冒烟（claw_minihub）

最短路径：Store 安装所需模块 → 登录/同步（若该模块需要）→ 控制点位。

`miot` 仅为可选示例；也可只装其它插件，或不装云插件。

---

## 前置

- 设备已联网
- MiniHub Store 可用
- 已打包目标插件（`components/store_modules/<id>` 或外部包）

## 步骤

1. **安装** — Store UI / `POST /api/store/install` `{"kind":"module","id":"<id>","reload":true}`
2. **确认加载** — 串口 `module --list` 或 Web `/api/modules`
3. **登录（若插件提供 auth RPC）** —  
   `module --rpc --id <id> --method <login|…> [--params …]` 或 Web / `hub.api`
4. **控制** — MCP `points.list` / `command.invoke`（`adapter` 用实际适配器名）
5. **验收** — 实体设备或点位 `online` 合理

### 可选：miot 示例

```text
module --rpc --id miot --method login --params {"username":"...","password":"..."}
module --rpc --id miot --method sync
```

## 失败快查

| 现象 | 处理 |
|------|------|
| `module not ready` / RPC 失败 | 确认已安装并 reload；`module --list` |
| 无点位 | 登录/同步是否完成；`adapters.get` |
| `HUB_NOT_READY` | 等待 Hub 就绪 |

## 状态

- [ ] 所选插件冒烟通过（id：____ 日期：____）

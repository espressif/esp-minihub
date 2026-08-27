# claw_minihub 操作文档

本目录为 **Claw 产品（微信 / Agent + MiniHub MCP）** 的用户与实验室操作说明。

开发文档见仓库根目录 [`docs/`](../../../docs/) 与 [`docs/plugins_and_store.md`](../../../docs/plugins_and_store.md)。

## 文档索引

| 文档 | 说明 |
|------|------|
| [Account-Binding.md](Account-Binding.md) | Store 插件安装 + 通用 `module` 串口 / Web 登录 |
| [Plugin-Smoke-Test.md](Plugin-Smoke-Test.md) | 冒烟：装插件 → RPC → 控制（插件可选） |

## 推荐顺序

1. 烧录：[`../README.md`](../README.md) · [`docs/build.md`](../../../docs/build.md)
2. （可选）`wechat --qr-start`
3. Store 安装需要的插件（可不装 / 可装多个）
4. `module --list` / `module --rpc …` 或 Web / `hub.api`

# adapter_miot 参考资料

本目录汇总 **小米 MIoT / 米家云控** 与 MiniHub `adapter_miot` 相关的公开资料、社区实现与项目内文档链接。

> **说明**：仓库内**未**包含小米官方 PDF/Word 协议手册（小米 IoT 开发者平台以 Web Spec 为主）。若你本地有合规获取的文档，可放入 `references/vendor/` 并在下方「本地文件」表中登记。

## MiniHub 实现范围（当前）

| 路径 | 说明 |
|------|------|
| `../miot_adapter.c` | 适配器：LAN miIO 优先，回退云控 |
| `../miio_lan.c` | UDP 54321 + AES，get/set_properties |
| `../auth/` | 云登录、签名、设备列表、miotspec 读写 |
| `../../docs/14-MIoT云接入Checklist.md` | 烧录后云接入操作清单 |

**云控 + 局域网 miIO**：登录后 `POST /api/miot/sync` 自动写入 `token`（及 `mac`/`ip` 提示）；`mode: auto` 优先 LAN + UDP 发现 IP；仅 `did` 无 token 时走云。

---

## 1. 小米官方 / 半官方（设备 Spec）

| 资料 | 链接 | 用途 |
|------|------|------|
| MIoT-Spec-V2 / 开发者平台 | https://iot.mi.com/ | 查设备 **model** 对应的 **siid / piid / 数据类型** |
| 小米 IoT 开发者文档（设备接入） | https://iot.mi.com/new/doc/ | 厂商接入流程；**非**第三方 Hub 云 API 手册 |

---

## 2. MiniHub 云 API 实现依据（社区逆向 + 自研）

以下域名与路径见 `components/minihub_plugins/miot/auth/miot_cloud.c`、`miot_cloud_2fa.c`：

| 域名 | 用途 |
|------|------|
| `account.xiaomi.com` | Passport 登录、`serviceLogin` / `serviceLoginAuth2`、2FA |
| `sts.api.io.mi.com` | STS 跳转（登录链） |
| `api.io.mi.com` | 中国区云 API 基址 `{region}.api.io.mi.com` 用于海外区 |
| `/home/device_list` | 设备列表 |
| `/miotspec/prop/get` | 读属性 |
| `/miotspec/prop/set` | 写属性 |

详细列表见 [`cloud-api-endpoints.txt`](cloud-api-endpoints.txt)。

**性质**：非小米对外公开的「第三方 Hub REST 文档」，为社区长期逆向与各开源项目对齐的行为；MiniHub 按 [`docs/05-集成插件规范.md`](../../../docs/05-集成插件规范.md) **自研**，不得复制 `ha_xiaomi_home` 源码。

---

## 3. 社区工具与参考实现

| 项目 | 链接 | 说明 |
|------|------|------|
| MiService | https://github.com/Yonsm/MiService | 导出 `userId` / `serviceToken` / `ssecurity`；MiniHub `POST /api/miot/session` |
| python-miio | https://github.com/rytilahti/python-miio | **局域网 miIO** 事实标准实现（MiniHub 未采用） |
| python-miio 文档 | https://python-miio.readthedocs.io/ | miIO 协议、token、本地控制 |
| Home Assistant Xiaomi Miio | https://www.home-assistant.io/integrations/xiaomi_miio/ | 设备兼容性与 token 获取经验 |

---

## 4. 局域网 miIO（已实现）

| 资料 | 链接 |
|------|------|
| 社区协议说明 | 见 [`lan-miio-community.txt`](lan-miio-community.txt) |
| 配置示例 | 见 [`lan-device.example.txt`](lan-device.example.txt) |
| UDP 端口 | **54321** |
| 发现 | 明文广播 `miio.info` → 按 token 加密握手绑定 IP |
| 认证 | 每设备 **32 字节 token**（需从米家/备份提取） |

---

## 5. MiniHub 项目内文档

| 文档 | 路径 |
|------|------|
| MIoT 云接入 Checklist | [`application/minihub/docs/MIoT-Cloud-Checklist.md`](../../../application/minihub/docs/MIoT-Cloud-Checklist.md) |
| 集成插件规范 §4.3 | [`docs/05-集成插件规范.md`](../../../docs/05-集成插件规范.md) |
| 技术选型 §8 | [`docs/07-技术选型与资源评估.md`](../../../docs/07-技术选型与资源评估.md) |
| 动态模块 / miot.mhub | [`docs/06-动态模块与Store.md`](../../../docs/06-动态模块与Store.md) |

---

## 6. 本地文件（txt）

| 文件 | 内容 |
|------|------|
| [`links.txt`](links.txt) | 全部 URL 纯文本列表 |
| [`cloud-api-endpoints.txt`](cloud-api-endpoints.txt) | MiniHub 已用云 HTTP 端点 |
| [`lan-miio-community.txt`](lan-miio-community.txt) | 局域网 miIO 社区资料索引 |
| [`minihub-miot-api.txt`](minihub-miot-api.txt) | Hub REST `/api/miot/*` 速查 |

## 7. 本地文件（PDF / Word）

当前目录 **无** 预置 PDF/DOC。可自行添加至 `references/vendor/`（勿提交含个人 token 或账号信息的文件）。

用途	URL
model → URN 索引
https://miot-spec.org/miot-spec-v2/instances?status=all
某设备完整 Spec
https://miot-spec.org/miot-spec-v2/instance?type=<URN>
人看的站
https://home.miot-spec.com / https://iot.mi.com

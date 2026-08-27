# claw_minihub (ESP-Claw + MiniHub Core)

Application shell based on esp-claw `edge_agent`. ESP-Claw is the git submodule [`components/esp-claw/`](components/esp-claw/) (`git submodule update --init application/claw_minihub/components/esp-claw`); path deps live in `main/idf_component.yml`.

**Store plugins** (e.g. optional `miot`) are **not** linked into this firmware. Install any needed module via Store after flash. Lab serial (plugin-agnostic):

```text
module --list
module --rpc --id <plugin_id> --method <name> [--params <json>]
```

**用户操作文档：** [docs/](docs/)。**开发文档：** 仓库根目录 [`docs/`](../../docs/) · [`docs/plugins_and_store.md`](../../docs/plugins_and_store.md)。

Build from repo root:

```bash
./tools/build_firmware.py -app claw_minihub build
```

## Quick Start

### Prerequisites

- ESP-IDF is installed and exported
- ESP-IDF **6.2** (S31) or **5.5.x** (S3 smoke) as required by your board

```bash
. <your-esp-idf-path>/export.sh
```

To use `idf.py bmgr -c` / `-b`, install the board-manager helper once per ESP-IDF environment:

```bash
pip install esp-bmgr-assist
```

### Configuration

1. Generate board support files:

```bash
cd application/minihub_host
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1
```

> `idf.py bmgr -c ./boards -b <board_name>` generates the configuration for the specified board. Available board names can be found in the `boards` directory.

2. Configure Wi-Fi, LLM, IM, search engine, and related parameters:

The key demo settings include:

- Wi-Fi SSID / Password
- LLM API Key / Provider / Model
- QQ App ID / App Secret
- Telegram Bot Token
- Brave / Tavily Search Key
- Timezone

Key Notes:

- IM bot token: available from Telegram [@BotFather](https://t.me/BotFather) or [QQ Bot](https://q.qq.com/qqbot/openclaw/login.html)
- LLM API key: available from [Anthropic Console](https://console.anthropic.com), [OpenAI Platform](https://platform.openai.com), or [Alibaba Cloud Bailian](https://bailian.console.aliyun.com/#/api-key)

You can adjust compile-time default values through `menuconfig`:

```bash
idf.py menuconfig
```

3. Build and flash:

```bash
idf.py set-target esp32s3   # or esp32s31 when IDF supports it (see below)
idf.py build
idf.py flash monitor
```

> **ESP32-S31:** Use ESP-IDF **6.2-dev** (or any IDF that lists `esp32s31` as preview/supported). On this machine: `/Users/esp/workspace/project/esp-idf-6.2-e9da155`. Commands need `idf.py --preview …`. Board assets: `boards/espressif/esp32_s31_korvo1/`. S3 compile smoke still uses `esp32_S3_DevKitC_1` + IDF 5.5.4.

### Build (ESP32-S31 Korvo1)

When your ESP-IDF supports S31 (preview on IDF 6.2-dev):

```bash
. /Users/esp/workspace/project/esp-idf-6.2-e9da155/export.sh
export IDF_EXTENSION_ALLOW_UNTRUSTED=1
export ESPBAUD=2000000   # default 2Mbps flash (or use ./idf.sh)
cd application/minihub_host
idf.py --preview bmgr -c ./boards -b esp32_s31_korvo1
idf.py --preview set-target esp32s31
idf.py --preview build
# after plugging Korvo1:
idf.py --preview -p <PORT> flash monitor
# equivalent: ./idf.sh --preview -p <PORT> flash monitor
```

Flash baud defaults to **2Mbps** via `ESPBAUD` / `./idf.sh` (IDF’s built-in default is 460800 and is too slow for fatfs images).
MVP Kconfig: WeChat IM + MCP client/server are enabled in `sdkconfig.defaults`. Partition table is **16MB only** (`partitions.csv`).

Firmware goal: Wi-Fi + `app_claw_start` + MiniHub Core bring-up (`minihub_bringup_start`).

### Claw Agent + WeChat + MiniHub MCP (Task 9)

The on-device agent controls hub devices only through the **local MiniHub MCP server** (`minihub_mcp_server` registered on `cap_mcp_server`). A FATFS skill at `fatfs_image/storage/skills/minihub_hub/SKILL.md` instructs the model to use `points.*` and `command.invoke` via `cap_mcp_client`.

#### LLM API key (do not commit secrets)

Configure at runtime — **never** put API keys in git, `sdkconfig`, or committed NVS dumps.

| Method | Steps |
| --- | --- |
| **Web UI** (recommended) | Join device AP or LAN → open `http://<device-ip>/` → **LLM** tab → set provider/backend, model, base URL, and **API key** → Save. Stored in NVS namespace `app` (`llm_api_key`, etc.). |
| **Setup wizard** | First boot walks Wi-Fi → LLM → IM; same fields as the LLM page. |
| **Compile-time defaults** | `idf.py menuconfig` → **App Config** only supplies empty Wi-Fi/search defaults; leave LLM key blank in firmware. |

Supported backends match upstream esp-claw (Anthropic, OpenAI, Alibaba Bailian, etc.). `claw_core` starts only when `llm_api_key`, `llm_model`, and `llm_backend_type` are all non-empty after load.

#### WeChat IM

WeChat uses esp-claw **ClawBot** polling (`cap_im_wechat`). Enable cap: `CONFIG_APP_CLAW_CAP_IM_WECHAT=y` (already set in `sdkconfig.defaults`).

| Field | Where | Notes |
| --- | --- | --- |
| `wechat_token` | Web **IM** tab or setup wizard | Obtained via in-browser **WeChat QR login** on the IM page (token auto-fills on success). |
| `wechat_base_url` | IM tab | Default `https://ilinkai.weixin.qq.com` |
| `wechat_cdn_base_url` | IM tab | Default `https://novac2c.cdn.weixin.qq.com/c2c` |
| `wechat_account_id` | IM tab | Default `default`; set if your ClawBot account differs |

Save IM settings, reboot or wait for gateway start, then message the bound WeChat account. Inbound text routes to `claw_core` via router rule `im_any_message_agent` (see `fatfs_image/system/.recovery/router_rules/router_rules.json`).

See esp-claw [cap_im_platform](https://github.com/espressif/esp-claw) docs for gateway behavior; MiniHub adds no separate WeChat stack.

#### Store plugins / account login

**Ops:** [`docs/Account-Binding.md`](docs/Account-Binding.md) · [`docs/Plugin-Smoke-Test.md`](docs/Plugin-Smoke-Test.md)  
**Plugins:** [`docs/plugins_and_store.md`](../../docs/plugins_and_store.md)

1. Serial (optional): `wechat --qr-start` then `wechat --qr-status`
2. WeChat DM (optional): `/llm setup deepseek <token>`
3. Install needed Store plugin(s), then login via that plugin’s RPC / Web UI:
   - Serial: `module --list` then `module --rpc --id <id> --method …`
   - Web / `hub.api`: `POST /api/modules/<id>/rpc`
4. Natural language device control via MCP (`points.list` / `command.invoke` on whatever adapters are loaded)

Do not send passwords or 2FA codes in group chats. Claw does not bake vendor login into firmware.

#### MCP client → local MiniHub server

`cap_mcp_client` tools: `mcp_list_tools`, `mcp_call_tool`, `mcp_discover`.

| Setting | Value |
| --- | --- |
| **Preferred URL** | `http://127.0.0.1:18791` (loopback on the same chip — server and client coexist) |
| **Endpoint** | `mcp` (default when omitted; matches `MCP_MDNS_DEFAULT_ENDPOINT`) |
| **LAN / mDNS** | Server advertises `_mcp._tcp`; `mcp_discover` with `include_self: true` returns `server_url` and port **18791** |

Agent flow: activate skill `minihub_hub` → `mcp_call_tool` with `server_url` above → `points.list` / `command.invoke`. On JSON error `HUB_NOT_READY`, hub bring-up is incomplete — ask the user to retry shortly.

External tools (desktop IDE) on the same LAN may use the mDNS hostname (default `esp-claw.local`) or STA IP with port 18791.

### MiniHub config storage and networking

- **Wi-Fi:** Claw `wifi_manager` owns STA/AP. `CONFIG_MINIHUB_NET_WIFI_STA=n` in `sdkconfig.defaults.minihub` so `minihub_net` does not start a conflicting stack. MIoT/Zigbee adapters use lwIP once Claw has connected.
- **Config files:** MiniHub v1 expects LittleFS at `/data` (`adapters.json`, `rules.json`). Claw boards use a FAT **storage** partition (mounted by `app_fs` at `/fatfs`), so `minihub_config_init` may log `config LittleFS unavailable, using embedded defaults` and run with in-firmware `DEFAULT_ADAPTERS` / `DEFAULT_RULES` until `/data` is wired or JSON is copied under a compatible mount.

## FATFS Image Layout

Source content for all FAT partitions lives under a single tree, with one
subdirectory per partition:

```text
application/minihub_host/fatfs_image/
├── storage/   # → storage partition (writable, mounted at /fatfs)
└── system/    # → system partition (read-only seed, mounted at /system)
```

Each subdirectory is copied into its own build-time staging directory:

```text
application/minihub_host/build/fatfs_image/        # storage staging
application/minihub_host/build/system_fs_image/    # system staging
```

Each board can also provide optional board-specific FATFS content under its own board directory. This content is overlaid onto the `system` partition:

```text
application/minihub_host/boards/<vendor>/<board>/fatfs_image/
```

During the build, `application/minihub_host/CMakeLists.txt` first copies the base `fatfs_image/system/` directory into the system staging dir, then copies the selected board's `fatfs_image/` directory if it exists. The selected board path comes from the generated `components/gen_bmgr_codes/CMakeLists.txt`, which is produced by `idf.py bmgr`.

If a board-specific file has the same relative path as a base system file, the board-specific file overwrites the base file in `build/system_fs_image/`. This lets a board replace firmware-baked defaults such as skills, scripts, and static assets without changing the shared base image. Board `fatfs_image/` content targets the SYSTEM image only; hidden board folders are not considered.

Skill manifests and built-in Lua scripts/docs are synced into `build/system_fs_image/` so they end up on the read-only system partition; the writable storage partition can be reformatted at runtime and re-seeded from `/system` without losing them.

---

## Upstream esp-claw

Claw sources are the git submodule `application/claw_minihub/components/esp-claw` (see `.gitmodules`). To browse upstream docs or `edge_agent` samples in isolation, clone [espressif/esp-claw](https://github.com/espressif/esp-claw) separately — do not add it under MiniHub `third_party/`.

For MiniHub Claw firmware, always build with `./tools/build_sdk.py --app claw_minihub` (or from `application/claw_minihub/` with IDF exported).

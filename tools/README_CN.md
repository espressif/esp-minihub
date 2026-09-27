# 工具

[English](README.md) | [中文](README_CN.md)

产品目录仅为 `application/<name>/`（默认 `minihub`）。更多说明：[docs/build.md](../docs/build.md)、[docs/plugins_and_store.md](../docs/plugins_and_store.md)、[docs/module_sdk/README.md](../docs/module_sdk/README.md)。

多个脚本共用的环境变量：

| 变量 | 默认 | 含义 |
|------|------|------|
| `MINIHUB_APP` | `minihub` | `application/` 下的产品目录 |
| `MINIHUB_PLUGINS_DEPLOY_DIR` | `~/workspace/minihub_store` | Store 目录 + `<id>/<id>.mhub` |

---

## Dev sync 面板（本地可视化推送）

### `dev_sync.py` + `dev_sync.html`

浏览器面板，向设备上传/下载：**Web**（html/css/js）、**adapters.json**、**类型目录 CSV**（`devices.csv` / `parameters.csv`）。设备 REST 无 CORS，因此用本机小服务提供 UI，并把 `/hub/*` 代理到设备。不要直接用 `file://` 打开 HTML。

```bash
python3 tools/dev_sync.py --open          # http://127.0.0.1:8765/
python3 tools/dev_sync.py --port 9000
# 可选默认：MINIHUB_DEV_BASE=http://192.168.1.50 MINIHUB_DEV_TOKEN=...
```

页面填写设备地址（如 `http://minihub.local`）与可选 Bearer。需固件 `CONFIG_MINIHUB_WEB_UI`；鉴权与控制台 REST 相同。

| 资源 | 设备 API | 设备路径 |
|------|----------|----------|
| Web | `PUT /api/dev/web/<rel>`；读 `GET /web/<rel>` | `/store/preinstall/web/` |
| adapters | `GET/PUT /api/config/adapters` | `/data/config/adapters.json` |
| 类型目录 | `GET/PUT /api/dev/catalog/{devices,parameters}.csv` | `/data/config/model/` → 扁平 `/data/config/devices.csv` + reload |
| 图标 | （面板标注即将支持） | `/data/images/…` |

类型目录说明见 [`components/minihub_device_widget/README_CN.md`](../components/minihub_device_widget/README_CN.md#remote-catalog)。

---

## 固件

### `build_firmware.py`

统一的产品 IDF 构建入口。其余参数转给 `idf.py`。

```bash
./tools/build_firmware.py [-app NAME] [stacks...] [--] [idf.py args...]
./tools/build_firmware.py module [all|<id> ...]    # 编译原生模块 .so
./tools/build_firmware.py modules                  # 编译全部原生 .so
./tools/build_firmware.py pack [all|<id> ...]      # 只打包 .mhub，不编译
./tools/build_firmware.py merge                    # 合并烧录镜像
```

协议栈（小写）：`ble_mesh`、`zigbee`、`matter`、`thread`。`thread` 与 `zigbee` 互斥。省略时默认：`zigbee ble_mesh`。

```bash
./tools/build_firmware.py -app minihub zigbee ble_mesh build
./tools/build_firmware.py thread matter build
./tools/build_firmware.py build merge
./tools/build_firmware.py module color_bulb
./tools/build_firmware.py modules
./tools/build_firmware.py pack color_bulb
```

### `apply_sdkconfig_variant.sh`

把 `application/<app>/config/sdkconfig/variants/<name>.yml` 覆盖到 `app.yml`。

```bash
./tools/apply_sdkconfig_variant.sh lcd
./tools/apply_sdkconfig_variant.sh --restore    # variants/default.yml
```

### `compose_sdkconfig.py`

固件构建用的库：把 `app.yml` + feature 片段展开为 `SDKCONFIG_DEFAULTS`。不是日常 CLI。

### `sync_host_manifest.py`

合并 `config/minihub_manifest.yml`（`_host_base`）与 `application/<app>/manifest.yml` → 组件 require / adapters 配置。

```bash
python3 tools/sync_host_manifest.py --app minihub
```

### `product_assets.py`

解析产品配置 JSON（先 `application/<app>/config/`，再仓库 `config/`），以及 `manifest.yml` → `store_preinstall` 的工厂 Store id。

```bash
python3 tools/product_assets.py --app minihub resolve adapters.json
python3 tools/product_assets.py --app minihub store-preinstall
python3 tools/product_assets.py --app minihub seed-config /tmp/hub_config
```

### `apply_component_patches.sh`

managed-component 重新下载后重打 `patches/`。补丁头可选 `Apply-Root:`。

```bash
./tools/apply_component_patches.sh
./tools/apply_component_patches.sh --check    # dry-run；需要打补丁时 exit 1
```

---

## Store 插件

Id：小写 + 下划线，禁止连字符（`^[a-z][a-z0-9_]{0,30}$`）。

### `new_store_module.py`

复制 `docs/module_sdk/templates/l{1,2,3}/` → `examples/plugins/<id>/`。

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack --name "My pack"
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native --force
```

### `build_plugin_standalone.py`

不依赖 IDF，编译 `examples/plugins/<id>` 下的 L3 演示（CMake + Plugin SDK）。

```bash
python3 tools/build_plugin_standalone.py color_bulb
python3 tools/build_plugin_standalone.py meter_plug --clean --pack
```

需要 PATH 上有 `riscv32-esp-elf-gcc`（或 `--cc` / `MINIHUB_PLUGIN_CC`）和 `cmake`。
CMake 树与 MHMD：`examples/plugins/<id>/build/<id>.so`（任意深度的 `build/` 已忽略）。
Host 树内 MHMD 仍在 `application/$MINIHUB_APP/build/<id>.so`。共用逻辑：
`cmake/minihub_plugin_standalone.cmake`（文档中也可整段复制到第三方工程）。

### `pack_store_module.py`

外层 `.mhub` 是 zip。`mhmd` 给已链接的 `.so` 加 MHMD 头（不是 zip）。

```bash
python3 tools/pack_store_module.py pack my_js
python3 tools/pack_store_module.py pack all --deploy ~/workspace/minihub_store
python3 tools/pack_store_module.py mhmd build/foo.so build/foo_mhmd.so
```

Host 只解 zip（`hub_zip.c`）。`--format` / `MHUB_PACK_FORMAT` 预留；目前仅实现 `zip`。

### `build_store_modules.py`

Host 构建后：把 `application/<app>/build/modules/libadapter_<id>.so` 打成 MHMD + `.mhub`。

```bash
python3 tools/build_store_modules.py all
python3 tools/build_store_modules.py my_native --deploy ~/workspace/minihub_store
```

也可：`./tools/build_firmware.py pack`。

### `sync_store_catalog.py`

重写部署目录的 `catalog.json`。无参数时默认只遍历 `DEPLOY/ID/ID.mhub`（不需要源码，也不看 `MINIHUB_PLUGIN_DIRS`）。`--from-store` 与默认相同。传入 `all` 或模块 id 会先按源码打包，再同样重写 catalog。该脚本也会打进 `module_sdk.zip`。

```bash
python3 tools/sync_store_catalog.py
python3 tools/sync_store_catalog.py --deploy ~/workspace/minihub_store
python3 tools/sync_store_catalog.py all
python3 tools/sync_store_catalog.py my_js --deploy ~/workspace/minihub_store
```

### `store_server.py`

局域网 Store HTTP（默认 `:8080`）：`catalog.json` 与 `/files/<id>.mhub`。

```bash
python3 tools/store_server.py --port 8080
python3 tools/store_server.py --deploy ~/workspace/minihub_store
```

Hub Store 指向该主机（`custom_url`），然后 refresh + install。

### `minihub_plugin_mcp.py`

stdio **IDE MCP**，供插件作者（Cursor / VS Code）。不是 `CONFIG_MINIHUB_MCP_SERVER`。

```bash
python3 tools/minihub_plugin_mcp.py
python3 tools/plugin_mcp/test_plugin_mcp.py
```

仓库接线：`.agent/mcp/cursor.mcp.json` 与 `.agent/mcp/vscode.mcp.json`，软链到 `.cursor/mcp.json` 和 `.vscode/mcp.json`。工具：`plugin_guide`、`scaffold_plugin`、`validate_plugin`、`pack_plugin`。实现：`tools/plugin_mcp/`。

### `store_paths.py`

共享路径助手（`DEMO_PLUGINS`、部署目录）。仅供 import。

### `check_module_so_layout.py`

检查 `libadapter_*.so` 的 PT_LOAD 布局是否适合 elf_loader。链接加
`-Wl,-z,max-page-size=4`。

```bash
python3 tools/check_module_so_layout.py application/minihub/build/modules/libadapter_my_native.so
python3 tools/check_module_so_layout.py path/to.so --warn-only
```

### `gen_host_export_symbols.py`

扫描 adapter `.so` 未定义符号 → Host export `.c` 与 `docs/module_sdk/host_export_allowlist.json`。新增 libc/IDF 符号且模块链接成功后运行。

```bash
python3 tools/gen_host_export_symbols.py \
  --modules-dir application/minihub/build/modules \
  --out components/minihub_module/host_elf_symbols_generated.c
```

---

## CI 与 git

| 路径 | 作用 |
|------|------|
| `ci/install_git_hooks.sh` | 安装 pre-commit + pre-push（+ 去掉 Cursor commit trailer） |
| `git-hooks/commit-msg-strip-cursor` | 本地 commit-msg 助手 |

固件 / 打包 / 文档的 CI 任务在 `.gitlab-ci.yml` 里直接调用 `./tools/build_firmware.py` 和 `./tools/build_docs.sh`。

```bash
bash tools/ci/install_git_hooks.sh
./tools/build_firmware.py --app minihub build merge
```

---

## LCD 字体（`ui_font/`）

为 `components/minihub_lcd/ui/fonts/minihub_sc.otf` 子集化思源黑体。需要 `pyftsubset` 与 LVGL 自带 OTF（或 `SRC_FONT`）。

```bash
./tools/ui_font/build_subset.sh
MINIHUB_FONT_UI_ONLY=1 ./tools/ui_font/build_subset.sh   # 更小，仅 UI ASCII
```

`collect_chars.py` / `gen_chars_3000.py` 生成子集字符表；日常一般跑 shell 包装脚本。

仅 vendor 的助手（不在本目录）：`MINIHUB_PLUGIN_DIRS` 下插件自己的 `tools/`。

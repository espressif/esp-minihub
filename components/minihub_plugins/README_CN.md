# 可 Store 安装的模块

[English](README.md) | [中文](README_CN.md)

模块源码放在本目录同级子目录：`components/minihub_plugins/<id>/`。
以 Store `.mhub` 交付，**不是** Host `require`。

独立演示：`examples/plugins/<id>`（不在本目录做 symlink）。与 Host 一起编时设
`MINIHUB_PLUGIN_DIRS` —— 见各 demo README。

```bash
python3 tools/new_store_module.py --tier l1 --id my_pack
python3 tools/new_store_module.py --tier l2 --id my_js
python3 tools/new_store_module.py --tier l3 --id my_native
```

模板：`docs/module_sdk/templates/`。Skills：`.cursor/skills/minihub-plugin-l1` … `l3`。

工厂预装：`application/<app>/manifest.yml` → `store_preinstall: [<id>, …]` → 设备
`/store/modules/<id>/`。id 须在 `components/minihub_plugins/<id>/` **或**
`MINIHUB_PLUGIN_DIRS` 指向的目录中解析到。（`module.json` 的 `"preinstall"` 不用于
工厂打包。）

| 层级 | 典型内容 | 产物 |
|------|----------|------|
| L1 pack | `module.json`（+ inject） | `<id>.mhub` |
| L2 JS | `module.json` + `main.js`（+ UI） | `<id>.mhub` |
| L3 native | `module.json` + CMake / `.so` | MHMD `.so` + `<id>.mhub` |

厂商插件（小米 MIoT）在旁路仓库 `esp-minihub-plugins`，不在 `examples/plugins`。把 `MINIHUB_PLUGIN_DIRS` 指到那份克隆。

部署目录（`MINIHUB_PLUGINS_DEPLOY_DIR`，默认 `~/workspace/minihub_store`）：

```text
<deploy>/catalog.json
<deploy>/<id>/<id>.mhub
```

```bash
./tools/build_firmware.py -app minihub modules   # 编译全部原生 .so
./tools/build_firmware.py -app minihub pack      # 只打包已有 .so
python3 tools/pack_store_module.py pack all
python3 tools/sync_store_catalog.py all
```

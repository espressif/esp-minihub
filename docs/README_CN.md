# 文档源码目录

MiniHub **编程指南** 的 RST 源码，使用
[esp-docs](https://github.com/espressif/esp-docs) 构建（与
[esp-idf/docs](https://github.com/espressif/esp-idf/tree/master/docs)、
[esp-matter/docs](https://github.com/espressif/esp-matter/tree/main/docs)
相同工具链）。

本指南是 **开源二次开发的主文档**：架构与优势、OEM Host、Store 插件、量产/OTA、
REST/MCP/C/JS 契约 — 目标是不阅读 Core C 即可完成集成与扩展。

* 英文：[docs/en/](en/index.rst)
* 中文：[docs/zh_CN/](zh_CN/index.rst)

## 读者入口

| 目标 | 首读章节 |
|------|----------|
| 理解方案与优势 | [introduction.rst](zh_CN/introduction.rst) → [architecture.rst](zh_CN/architecture.rst) |
| 编译烧录 | [getting_started.rst](zh_CN/getting_started.rst) |
| OEM Host 产品 | [host_customization.rst](zh_CN/host_customization.rst) → [production.rst](zh_CN/production.rst) |
| Store 插件 (L1/L2) | [plugins/overview.rst](zh_CN/plugins/overview.rst) |
| 仅 REST / 自动化 | [type_catalog.rst](zh_CN/type_catalog.rst) → [data_model.rst](zh_CN/data_model.rst) → [api/](zh_CN/api/index.rst) |
| 参与贡献 | [contributing.rst](zh_CN/contributing.rst) |

## 构建

输出目录固定为 **`docs/_build/`**（须在 ``docs/`` 目录下运行 ``build-docs``）。
**不要**在仓库根目录使用 ``--source-dir docs``，否则会在根目录生成多余的 ``/_build/``。

在仓库根目录（推荐）：

```bash
python3 -m venv .venv-docs
source .venv-docs/bin/activate
pip install -r docs/requirements.txt
./tools/build_docs.sh          # 英文 + 中文
./tools/build_docs.sh zh_CN    # 仅中文
```

若已存在 ``.venv-docs``，``./tools/build_docs.sh`` 会自动启用它。

或在 ``docs/`` 内手动执行：

```bash
cd docs
pip install -r requirements.txt
build-docs -bs html -t esp32s31 -l en
build-docs -bs html -t esp32s31 -l zh_CN
```

HTML 输出：`docs/_build/en/esp32s31/html/` 与 `docs/_build/zh_CN/esp32s31/html/`。

构建 HTML 时，类型目录快照会随站点发布到 `_static/catalog/`（CSV、设备图标、
`STANDARD_TYPES*.md`、示例 `devices.json`）及 `_static/catalog.zip`，源文件来自
`components/minihub_device_widget/`（Sphinx 配置阶段复制）。
`docs/_extensions/omit_elf_plugin_docs.py` 里 ``HIDE_L3`` 为 true 时，HTML
不收录 L3 章节，以及包在 ``.. only:: not hide_l3`` 里的段落。改成
``HIDE_L3 = False`` 后重新发布。``.rst`` 源文件保留。

本地浏览示例：`docs/_build/zh_CN/esp32s31/html/index.html`。

HTTP 预览（先成功构建）：

```bash
python3 tools/preview_docs.py          # http://127.0.0.1:8765/zh_CN/
python3 tools/preview_docs.py --open # 同时打开浏览器
```

`/` 会跳到 ``/zh_CN/``（``--lang en`` 则跳英文）。改 RST 或 Doxygen 头文件后需重新 ``./tools/build_docs.sh``。

`build-docs` 把 Sphinx warning 当作失败（与 CI 一致）。避免在 `` `` 字面量里写
`<...>`（Sphinx 会当成 role）；占位符用 `APP` / `ID`。中文标题下划线按东亚字符宽度计，需长于标题显示宽度。

macOS 上的 Homebrew Doxygen 1.18 有时会在写出 XML 或警告日志之前以 SIGBUS 退出。这是 Doxygen 程序本身，不是 RST 错误。``./tools/build_docs.sh`` 对这次崩溃自动重试一次。同一语言连失败两次时，再单独执行 ``./tools/build_docs.sh en`` 或 ``./tools/build_docs.sh zh_CN``。

## 中文术语（Matter）

| 英文 | 推荐中文 | 避免 |
|------|----------|------|
| Matter controller / commissioner | Matter 控制器（含配网） | 委员端 |
| commissioning | 配网 | 配对（易与蓝牙配对混淆） |
| commissioner build | 启用 Matter 控制器的固件构建 | 委员端构建 |

## 目录

```
docs/
  README.md / README_CN.md
  requirements.txt
  conf_common.py
  _static/
  _build/             # esp-docs HTML 输出（git 忽略）
  en/                 # 英文 RST
  zh_CN/              # 中文 RST（相对路径与 en/ 对齐）
  module_sdk/         # Host 导出符号白名单
  superpowers/        # 内部设计笔记（不进入 Sphinx toctree）
```

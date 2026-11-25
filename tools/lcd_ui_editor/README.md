# LCD UI 编辑器

树形编辑器，改 **设备小屏 UI** 的 Brookesia JSON 资产
（`components/minihub_lcd/ui/`），不用手改 JSON。

## 它改什么 / 不改什么

| 路径 | 本工具 |
|------|--------|
| `components/minihub_lcd/ui/screens/*.json` | ✅ 可改（`viewScreen`） |
| `components/minihub_lcd/ui/templates/*.json` | ✅ 可改（`viewTemplate`） |
| `components/minihub_lcd/ui/flows/*.json` | ✅ 可改（`screenFlow`） |
| `components/minihub_lcd/ui/constants/` `assets/` | ✅ 可改 |
| `components/minihub_lcd/ui_contract.json` | ❌ 不改：它是 C++ 侧的名字注册表，不是画面树 |
| `components/minihub_lcd/minihub_lcd_app.cpp` | ❌ 不改：C++ 只按生成的宏引用名字，不写死路径 |

画面树与 C++ 的关系：画面长什么样看 `ui/*.json`，C++ 用哪个节点看
`ui_contract.json`（由 `tools/gen_lcd_ui_contract.py` 生成 `ui_contract.gen.h`）。
**改了画面节点 id，必须同步改 contract**，否则 contract 测试会拦住保存。

## 用法

```bash
python3 tools/lcd_ui_editor/server.py            # 起服务并打开浏览器
python3 tools/lcd_ui_editor/server.py --port 8791 --no-open
```

只监听 `127.0.0.1`。纯标准库，无第三方依赖。

## 界面

顶栏两个模式：**结构编辑** 与 **屏幕预览**（快捷切换）。

**结构编辑**（左树 + 右表单）

- **左树**：按 `screens / templates / flows / constants / assets` 分组，点节点选中。
- **右栏**：选中节点的属性表单 —— `type`、`id`、文本（`labelProps.text`）、
  样式（`style`，带取色器）、布局（`layout`）、位置（`placement`）、
  通用（`commonProps`）、绑定（`bindings`）、事件（`events`，动作名带下拉候选）。
- **节点操作**：上移 / 下移 / 复制 / 删除 / 添加子节点（container / label / button / image）。
- **原始 JSON**：单节点的完整 JSON，`应用到该节点` 覆盖 —— 兜底通道，
  遇到表单没覆盖的字段直接在这里改。

**屏幕预览**

- 顶部一排按钮切换 6 个屏；下面按固件当前分辨率（默认 **800×480**）画出画面。
- **点画面里的元素 → 自动跳回结构编辑并选中该节点**（面包屑给出完整路径）。
- 空文本的槽位用虚线标出（运行时才填充的，如商店行、设备卡）。
- `image` 用斜纹占位并显示 `imageProps.src`（`${image.device}` 这种占位符）。
- 第二排可以**切换目标屏宽**（预设 480/640/800/960/1280 或自定义），用来看
  同比例、不同分辨率的屏上布局对不对。

### 预览的精度（重要）

预览是**按 Brookesia 布局规则做的近似渲染**，不是 Brookesia 引擎本身。覆盖的是
资产真正用到的那部分规则：

| 维度 | 支持 |
|------|------|
| `placement` | `absolute`（x/y/width/height）、`relative` + `align:center`、`flexGrow` |
| `layout` | `flex`、`flexFlow` column/row/rowWrap、`mainAlign` start/center/end/spaceBetween、`crossAlign` start/center/end/stretch、`gap` |
| `style` | `bgColor`、`borderWidth`/`borderColor`、`padding`、`radius`、`textColor`、`fontSize` |
| `commonProps` | `hidden`（不画）、`scrollable`（虚线标出） |
| 节点类型 | `container`、`label`、`button`、`image` |

**文字宽度用浏览器字体量的**，与真板（Montserrat + 内置 CJK）有差异，
所以**用来看布局，不要拿它判断逐像素排版**；真效果仍以上板为准。

已按资产里的 `placement` 逐项核对过几何：sidebar `(0,0) 112x480`、
overview_main `(112,0) 688x480`、导航按钮 `100x44`、统计卡 `160dp`、
过滤芯片 `56x32` —— 与 JSON 里的值一致。

### 切换分辨率（density）

**基准几何不写死在工具里**：后端 `screen_profile()` 从
`components/minihub_lcd/minihub_lcd_app.cpp` 解析
`kShellSidebarW` / `kShellMainW` / `kShellScreenH` 和 `make_environment()` 的
`env.density` / `env.font_scale`，经 `/api/ui` 的 `screen_profile` 字段给前端。
固件改了这些常量，预览会跟着变，不会出现两份 800×480。

选一个目标屏宽 = 模拟另一块同比例屏上的 `env.density = 屏宽 / 基准宽`：

| 值 | 预览中的处理 | 对应 Brookesia |
|----|--------------|----------------|
| `12dp` | `12 × density` | `dp` 乘 `environment.density` |
| `16sp` | `16 × density × font_scale` | `sp` 乘 `density * font_scale` |
| `50%` | 按父容器尺寸解析，不缩放 | 相对值 |
| `32px` | 原样 | 显式像素不缩放 |
| `flexGrow: 1` | 原样（无单位比例） | 不是长度 |

预览会按 `1/density` 自动调整缩放比，**不同分辨率在屏幕上看起来一样大**——
这本来就是同比例 + 等比缩放的正确结果，所以真正要看的是**没跟着缩放的东西**。

勾选「**显示 C++ 固定几何**」会叠加橙色虚线框，标出
`pin_shell_geometry()` 用裸 `lv_coord_t` 钉死的 sidebar / main 位置。
`density ≠ 1` 时这些框会明显和真实布局错开 —— 那是换屏时必须先修的地方
（常量要乘 density，或让 JSON 自己管）。

**预览没有建模的设备端差异**（真效果仍以上板为准）：

- `imageSet` 里的图标是固定 32×32 位图，`parser_assets.cpp` 不读 density；
- CJK 仅 FreeType（`sc_14` / `minihub_sc.otf`），无内置 Han Sans fallback；缩放行为未实测；
- 内置 Montserrat 有 8~28 的字号阶梯，后端会取"最接近且更小"的一档。


## 两条硬保证

**1. 校验权威是既有 contract 测试，不另立一套规则。**
保存时后端跑 `tools/test_lcd_ui_contract.py`：contract 声明的每个
view/action 必须真在 `ui/*.json` 里、C++ 不得写死路径、模板子节点要对得上……
规则只有这一份，不会和固件契约漂移。

**2. 保存是事务性的。**
先备份 → 写入 → 跑 contract 测试 → **失败就回滚并拒绝保存**（HTTP 422）。
所以坏掉的 UI 不会留在磁盘上。

### 失败归因：谁弄坏的？

契约套件同时检查**固件 C++**（硬编码路径/动作名、刷新接线、字体、flash 写入钩子）。
改 JSON 资产**不可能**弄坏这些，所以：

| 失败位置 | 保存 |
|----------|------|
| `ContractStructureTest` / `ContractCoverageTest` / `DeviceTemplateTest`（读资产与 contract） | ❌ 拒绝并回滚 |
| 只在固件 C++ 相关类里失败 | ✅ 允许保存，但状态栏会如实报出失败项，并说明「与本次改动无关」 |

这样别的 agent 在途的 C++ 改动不会把所有人锁在编辑器门外，而 UI 契约一旦被
破坏照样拦得住。**顶栏 `校验` 按钮跑的是完整套件**，可以看到全部失败。

## 不制造格式噪声

UI 资产里混着三种排版（完全展开、`{"a": 1}`、`{ "a": 1 }`），且分属不同作者。
写回器按**每个容器原有风格**重新输出，并且：

- 解析后内容与磁盘一致的文件 **完全不重写**（空保存 = 0 文件改动）；
- 文件末尾换行有无也照原样保留。

因此改一个标签只产生**一行** diff，不会把整个文件重排。13 个现有资产全部
通过字节级往返校验（见 `tools/test_lcd_ui_editor.py`）。

另外，**属性面板只读不改模型**：选中节点不会给它补上空的 `events: []` /
`children: []` / `labelProps: {}` —— 那些容器只在真正写入时才挂上去。
`select()` 里有一道金丝雀，一旦渲染意外改了模型就会立刻在控制台报错。

## 已知限制

- **不是像素级预览**：预览按 Brookesia 的 flex/absolute 规则近似渲染，
  但字体度量、图标位图、CJK 字号都不跟真板一致；也**不可拖拽摆放**。
  用来判断布局，真效果仍要上板（`CONFIG_MINIHUB_LCD_UI`）。
- **标签文字直接写在 JSON 里**（当前是中文），没有 i18n 机制；改文案即改 JSON。
- **动作名必须已存在**：事件里的 action 要在 `ui_contract.json` 或 flow 的
  transitions 里，否则固件没有对应处理函数。编辑器的下拉只提示已有名字，
  但不强拦新名字 —— 固件侧是否实现仍要看 `minihub_lcd_app.cpp`。

## 自测

```bash
python3 -m unittest tools.test_lcd_ui_editor -v
```

在线核对「空保存零改动」（需要先起服务）：

```bash
python3 tools/lcd_ui_editor/server.py --port 8791 --no-open   # 另一个终端
python3 tools/lcd_ui_editor/check_noop_save.py --port 8791
# 期望：saved 0 changed file(s) of 13 checked | 改动: 无
```

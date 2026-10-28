---
name: minihub-docs
description: Write or update MiniHub programming guide (esp-docs RST en/zh_CN, or Markdown fallback). Use when the user edits docs/, RST chapters, API reference, translations, or docs CI.
---

# MiniHub documentation

## Which tree

- Edit **en + zh_CN** with the same relative `.rst` path (`docs/en/`, `docs/zh_CN/`).
- Folder map: `docs/README.md`.

## Voice

Ruled 2026-10-23 (three-party review, `REVIEW_qoder_omit_elf_plugin_html_20261023_0045.md` Q18):

- An explanatory paragraph keeps the reason **only when it changes what the reader does next**
  (a product that copies Core stops receiving fixes; lab Wi-Fi credentials belong in
  ``local.overrides``, not in the factory ``adapters.json``). State the fact, then the consequence.
- Reference content — field tables, Kconfig lists, per-option bullets — uses plain declarative
  sentences, no reasons attached.
- Never make the reason a paragraph-ending formula. A page where every paragraph closes with
  "because …" / "因此 …" is the tell, not the cause; the human reference (ESP-IDF guide) runs
  about 6-7 causal connectives per 100 sentences, not one per paragraph and not zero.
- Reader and plugin author are **developer / 开发者**. 厂商 / `vendor` stays only where it means the
  device brand (the `manufacturer` field, the detail page's 厂商 / 型号 / 版本, `module.json`'s
  `vendor` key, Zigbee Basic 0x0004).

## Build RST

```bash
python3 -m venv .venv-docs   # first time
# ./tools/build_docs.sh activates .venv-docs when present
./tools/build_docs.sh        # en + zh_CN; warnings = failure
```

Needs `doxygen` on PATH for C API pages.

## Style

- RST: no ``literals`` containing `<...>`. CJK titles: underline at least as wide as the title.
- CJK inline markup (measured with docutils for `` x ``, **bold**, *em*, `:doc:`; Sphinx warnings are
  fatal here): an inline markup **end-string needs a real boundary after it** — whitespace, or
  `。 ， 、 ： ） ；`. An opening bracket `（ ( 「` is not a boundary, and neither is a CJK word
  character (`**粗**阻塞`). Insert a space. The reported line is the *paragraph* start, not the offender,
  and the same bad shape often renders clean when a later pair in the block rescues the open — so fix
  what the gate flags and re-run the gate; do not sweep by grep.
- `./tools/build_docs.sh` generates a target sdkconfig from the Host app, so it needs `IDF_PATH` and,
  whenever the feature list pulls in Matter, `ESP_MATTER_PATH` — otherwise it dies before Sphinx runs.
- Terminology, en and zh: **firmware / 固件** is the software a device runs; an **image / 镜像** is the
  artifact that gets flashed, verified or sized (the app binary in an OTA slot, a LittleFS image, a
  merged flash image). Slots hold images, devices run firmware — "the update failed" is about the
  image, "it will not boot" about the firmware. This matches Espressif's own zh_CN split
  (`esp-idf/docs/zh_CN`: 固件 182, 镜像 342, e.g. 应用程序镜像 / 引导加载程序镜像), so never flatten one
  word into the other. Two shapes to avoid: 镜像/`image` for a *build* ("不能出现在公开镜像里的凭证"
  → 公开构建), and 镜像 for the verb "mirror" (the event-bus sense in `source_tree`/`glossary` is fine,
  but read it as a verb). The mapping is stated in `glossary.rst` under *Firmware and image /
  固件 与 镜像*.
- Do not narrate the document or the figure. The test is the sentence's subject: if it is a thing in
  the system, keep it; if it is the act of writing or reading, cut it. Measured against
  `esp-idf/docs` (128 files each side): `it is worth` / 值得-as-editorialising appears **0 and 1** times
  there but 2 and 5 here — those five are gone (2026-10-26); `this page/chapter` (0.27/file en) and
  本章 (0.27/file zh) *are* reference-normal, so ledes like "This chapter says what the Host protects"
  stay. Shapes to delete, all of which told the reader nothing: 「所以值得说清时间从哪来」,
  「有三行值得读两遍：」, 「读表之前有三件事值得先知道：」, 「图随时间逐节点点亮，顺序与串口日志
  一致」, 「图与文字由 docs/_extensions/… 的同一张表生成」, 「竖着的位置说的是离用户多远」. Where the
  line was also carrying a fact (a colour legend, what a row means), rewrite it with the system as
  subject: 「其中三个状态码还带状态码本身看不出的行为：」, 「有两个窗口按设计是不设防的：」.
- Align with code:
  - Store `kind` is `module` or `pack`; Store config field `custom_url`
  - L1 / L2 / L3 are **Store packaging tiers**, not Host Core layers
  - Host = Core + bundled adapters; demos under `examples/plugins` + `MINIHUB_PLUGIN_DIRS`; default `store_preinstall: []`
  - Core type-agnostic: one device = one `minihub_point_t`, params in `props.params`; catalog CSV → `/data/config/devices.csv`; the widget set is `components/minihub_device_widget/model/ui_elements.csv` — read it, never copy the list or its size into a skill or a page
  - L2 `host.command.invoke` accepts `{ point_id, cmd, params? }`; use
    `host.command.on(adapter, fn)` and `point.register({ params|props })` for
    Devices-page ownership from JS
- Do not document `application/claw_minihub` or `CONFIG_MINIHUB_NET_PREFER_ETH`. Default IPv4 route: `minihub_net_refresh_default_netif()`.
- Host stays ecosystem-agnostic in prose (vendor examples belong in plugin chapters).

## Web UI first, REST second

Ruled 2026-10-23 (user decision): tutorials and getting-started steps describe the
**Web UI** as the primary way to verify and operate the device.

- Store install, catalog refresh, custom Store URL, point/adapter inspection, login
  and most device operations are written as Web UI steps
  (``http://HUB/`` → **Store** → set custom Store URL → refresh catalog → install).
- REST API / ``curl`` / ``POST`` / ``PUT`` blocks are **auxiliary only** — kept for
  automation, OEM integration, or when the API itself is the subject (the
  ``api/`` reference pages). Do not lead a tutorial step with a curl call when a Web
  UI path exists.
- When a chapter must show REST, frame it as secondary (e.g. "or via REST: …") and
  keep the Web UI step first.

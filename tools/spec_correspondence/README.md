# spec_correspondence

四合一对应表（BLE Mesh / Matter / Zigbee / MiniHub）的 spec 真源与生成脚本。

## 数据来源（2026-11-12 拉取，官方权威）
- Matter 设备类型（105）：connectedhomeip 自动生成的
  `ids_and_codes/spec_device_types.html`（每个含 hex ID + 名）。
- Zigbee/Matter ZCL 集群（159）：connectedhomeip 自动生成的
  `ids_and_codes/spec_clusters.html`（每个含 hex ID + 名）。这是 Matter 与 Zigbee
  共用的 ZCL 集群集合。
- Zigbee ZHA 设备 ID：Zigbee HA Public Profile (NXP) + Zigbee Lighting &
  Occupancy Device Spec (CSA) 片段。
- BLE Mesh 模型：SIG Model ID 表（cfanz / Silabs / Wikiwand）。

## 文件
- spec_matter_device_types.csv / spec_zcl_clusters.csv /
  spec_zigbee_zha_device_ids.csv / spec_ble_mesh_models.csv — 全量 spec 真源
  + MiniHub 支持标记（yes / param / no；集群表为 matter/zigbee 引用标记）。
- correspondence.md — 人读全量四合一表（§1 四合一统一对照表按 MiniHub 实体对齐；
  后续 §2–§7 为分协议全量明细 + 覆盖概览与缺口）。
- correspondence_matrix_devices.csv — §1.1 设备表的机器可读版（Matter 锚，105 行；adapt_status / minihub / ble_mesh / zigbee / matter）。
- correspondence_matrix_params.csv — §1.2 参数表的机器可读版（MiniHub 锚，36 行；同 5 列）。
- correspondence_params.csv / correspondence_devices.csv — 机器可读（MiniHub 侧）。
- minihub_mapping_raw.csv — 三张能力图解析出的 (协议, 种类, minihub键, 原生引用)。

## 如何扩展
1. 给三张能力图补 `classify` / `params` / `sensors` / `controls`。
2. 重跑 `python3 tools/spec_correspondence/build_correspondence.py`。
3. spec 真源如需更新：把 connectedhomeip 的两个 HTML 重新解析，替换脚本里的
   MATTER_DEVICES / ZCL_CLUSTERS 列表即可。

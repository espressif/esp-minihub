# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Fixed-limits tables, read back out of the code they document.

Every row names where the number lives — a ``#define``, a Kconfig ``default``, or
a regex anchor into the source. Rendering re-reads that source and refuses the
build when the value no longer matches, so a constant that moves cannot leave
a stale number in the guide (``./tools/build_docs.sh`` treats Sphinx warnings as
failure). The ``("line", rel, PATTERN)`` rows search the file for PATTERN (a regex whose
last ``(\\d+)`` group is the value, and whose optional leading group names a
stable anchor such as a function or symbol) instead of a hard-coded line number,
so neither the build nor the rendered citation couples to source line positions —
only a genuine value change is caught by verify().

Add a row when the guide quotes a cap. Keep the label to what the reader is
bounded by; put the explanation in the prose next to the table.
"""

from __future__ import annotations

import re
from pathlib import Path

from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.statemachine import StringList
from sphinx.errors import ExtensionError

from minihub_catalog_tables import _table

ROOT = Path(__file__).resolve().parents[2]

# group, en label, zh label, displayed value, numeric value, source spec
# source spec: ("define", rel, SYMBOL) | ("kconfig", rel, SYMBOL) | ("line", rel, PATTERN)
ROWS: list[tuple] = [
    ("core", "Registered points, all radios together", "所有射频合计的注册点位", "64", 64,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_MAX_POINTS")),
    ("core", "point_id length", "point_id 长度", "64", 64,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_POINT_ID_MAX")),
    ("core", "type string length", "type 字符串长度", "16", 16,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_TYPE_MAX")),
    ("core", "value string length", "value 字符串长度", "32", 32,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_VALUE_MAX")),
    ("core", "adapter name length", "适配器名长度", "16", 16,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_ADAPTER_MAX")),
    ("core", "command name length", "命令名长度", "32", 32,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_CMD_MAX")),
    ("core", "event name length", "事件名长度", "32", 32,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_EVENT_MAX")),
    ("core", "Command handlers", "命令处理函数", "16", 16,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_MAX_HANDLERS")),
    ("core", "Event-bus subscriptions", "事件总线订阅数", "32", 32,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_MAX_SUBS")),
    ("core", "Event queue entries before a drop", "事件队列积压后丢弃前的条数", "32", 32,
     ("define", "components/minihub_core/include/minihub_core.h", "MINIHUB_EVENT_QUEUE_DEPTH")),
    ("core", "Point history ring (all points share it)", "点位历史环形缓冲（全部点位共用）",
     "160", 160,
     ("define", "components/minihub_core/point_history.c", "POINT_HISTORY_GLOBAL")),
    ("core", "Entries one history read returns", "单次历史读取返回的条数", "64", 64,
     ("line", "components/minihub_core/point_history.c", r"if \(limit == 0 \|\| limit > (\d+)\)")),
    ("core", "State-store debounce window", "状态防抖窗口", "1000 ms", 1000,
     ("kconfig", "components/minihub_core/Kconfig.minihub", "MINIHUB_POINT_DEBOUNCE_MS")),
    ("core", "HTTP worker queue", "HTTP worker 队列", "4", 4,
     ("define", "components/minihub_core/http_worker.c", "MINIHUB_HTTP_QUEUE_LEN")),
    ("core", "Timezone offset range", "时区偏移范围", "±14 h", 840,
     ("define", "components/minihub_core/minihub_tz.c", "TZ_OFF_MAX")),
    ("core", "Timezone offset step", "时区偏移的粒度", "15 min", 15,
     ("line", "components/minihub_core/minihub_tz.c", r"if \(min % (\d+)\)")),

    ("rules", "Rules in one document", "一份文档内的规则数", "16", 16,
     ("define", "components/minihub_rules/minihub_rules.c", "MAX_RULES")),
    ("rules", "Time buckets", "时间桶", "16", 16,
     ("define", "components/minihub_rules/minihub_rules.c", "RULE_BUCKETS")),
    ("rules", "Rules per time bucket", "每个时间桶的规则数", "8", 8,
     ("define", "components/minihub_rules/minihub_rules.c", "MAX_RULES_PER_BUCKET")),
    ("rules", "if conditions per rule", "单条规则的 if 条件数", "4", 4,
     ("define", "components/minihub_rules/minihub_rules.c", "MAX_IF_CONDITIONS")),
    ("rules", "Steps per rule", "单条规则的步骤数", "5", 5,
     ("define", "components/minihub_rules/minihub_rules.c", "MAX_STEPS")),
    ("rules", "Actions per step", "每步的动作数", "4", 4,
     ("define", "components/minihub_rules/minihub_rules.c", "MAX_ACTIONS_PER_STEP")),
    ("rules", "Pending-delay timer slots", "延时待触发的定时器槽", "8", 8,
     ("define", "components/minihub_rules/minihub_rules.c", "RULE_TIMER_POOL")),
    ("rules", "Execution queue depth", "执行队列深度", "16", 16,
     ("define", "components/minihub_rules/minihub_rules.c", "RULE_EXEC_QUEUE_LEN")),
    ("rules", "Edge cache entries", "边沿缓存条目", "256", 256,
     ("define", "components/minihub_rules/minihub_rules.c", "EDGE_CACHE_MAX")),
    ("rules", "Time-trigger poll", "时间触发轮询", "1 s", 1000,
     ("define", "components/minihub_rules/minihub_rules.c", "TIME_CHECK_INTERVAL_MS")),

    ("scenes", "Scenes in one document", "一份文档内的场景数", "8", 8,
     ("define", "components/minihub_scenes/minihub_scenes.c", "MAX_SCENES")),
    ("scenes", "Steps per scene", "单个场景的步骤数", "5", 5,
     ("define", "components/minihub_scenes/minihub_scenes.c", "MAX_SCENE_STEPS")),
    ("scenes", "Actions per scene step", "场景每步的动作数", "8", 8,
     ("define", "components/minihub_scenes/minihub_scenes.c", "MAX_STEP_ACTIONS")),
    ("scenes", "Cancellable delay granularity", "可取消延时的粒度", "200 ms", 200,
     ("define", "components/minihub_scenes/minihub_scenes.c", "SCENE_DELAY_CHUNK_MS")),

    ("store", "Package size downloaded into PSRAM (.mhub)", "下载到 PSRAM 的包体积（.mhub）",
     "1 MiB", 1048576,
     ("kconfig", "components/minihub_module/Kconfig.minihub", "MINIHUB_HUB_MAX_BYTES")),
    ("store", "Files per package", "单包文件数", "64", 64,
     ("define", "components/minihub_store/hub_zip.c", "HUB_ZIP_MAX_FILES")),
    ("store", "Uncompressed budget per package", "单包解压后上限", "2 MiB", 2097152,
     ("define", "components/minihub_store/hub_zip.c", "HUB_ZIP_MAX_UNCOMP")),
    ("store", "Module id length", "模块 id 长度", "48", 48,
     ("define", "components/minihub_config/include/minihub_paths.h", "MINIHUB_MODULE_ID_MAX")),
    ("store", "Loaded native modules", "已加载的原生模块数", "12", 12,
     ("define", "components/minihub_module/module_loader.c", "MAX_MODULES")),
    ("store", "Bundled modules", "内建模块数", "4", 4,
     ("define", "components/minihub_module/module_loader.c", "MAX_BUILTIN")),
    ("store", "Host RPC slots", "Host RPC 槽", "8", 8,
     ("define", "components/minihub_module/module_loader.c", "MAX_HOST_RPC")),
    ("store", "File a module may read through the Host", "模块经 Host 读取单文件上限",
     "64 KiB", 65536,
     ("define", "components/minihub_module/module_loader.c", "MODULE_READ_FILE_MAX")),

    ("web", "Control request body", "控制请求体", "1 KiB", 1024,
     ("define", "components/minihub_web/minihub_web.c", "MINIHUB_WEB_CONTROL_BODY_MAX")),
    ("web", "rules.json and scene import payload", "rules.json 与场景导入体积", "128 KiB",
     131072, ("define", "components/minihub_web/minihub_web.c", "RULES_JSON_MAX_BYTES")),
    ("web", "adapters.json payload", "adapters.json 体积", "64 KiB", 65536,
     ("define", "components/minihub_web/minihub_web.c", "ADAPTERS_JSON_MAX_BYTES")),
    ("web", "Matter API request body", "Matter API 请求体", "1460 B", 1460,
     ("define", "components/minihub_web/minihub_web.c", "MATTER_API_BODY_MAX")),
    ("web", "WebSocket clients", "WebSocket 并发客户端", "8", 8,
     ("define", "components/minihub_web/minihub_web_ws.c", "WS_MAX_CLIENTS")),
    ("web", "URL OTA request body", "URL OTA 请求体", "256 B", 256,
     ("line", "components/minihub_web/minihub_web.c",
      r"(handle_api_ota_start)\(httpd_req_t \*\s*req\)\s*\{.*?char buf\[(\d+)\]")),
    ("web", "OTA upload chunk body", "OTA 上传分块请求体", "4 KiB", 4096,
     ("line", "components/minihub_web/minihub_web.c",
      r"(minihub_ota_upload_begin)\(expected\);.*?char buf\[(\d+)\]")),

    ("auth", "Web username", "Web 登录用户名", "admin", None,
     ("define", "components/minihub_auth/include/minihub_auth.h", "MINIHUB_WEB_USERNAME")),
    ("auth", "Idle session timeout", "空闲会话超时", "300 s", 300,
     ("define", "components/minihub_auth/include/minihub_auth.h", "MINIHUB_AUTH_SESS_SEC")),
    ("auth", "Absolute session cap", "会话最长存活", "8 h", 28800,
     ("define", "components/minihub_auth/include/minihub_auth.h", "MINIHUB_AUTH_SESS_ABS_SEC")),

    ("matter", "Matter tables (device / subscription / identity)", "Matter 表容量（设备/订阅/身份）",
     "128", 128, ("kconfig", "components/minihub_matter/Kconfig.minihub", "MINIHUB_MATTER_MAX_NODES")),
    ("matter", "Commissioned peers the controller keeps", "控制器实际保留的已配网对等节点",
     "8", 8, ("define", "components/minihub_matter/matter_project_config.h",
              "CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES")),
    ("matter", "Endpoints per node, and per commissioning", "单节点端点数 / 单次配网端点数",
     "16", 16, ("line", "components/minihub_matter/matter_ctrl.cpp", r"(k_pending_ep_max) = (\d+)")),
    ("matter", "Commission watchdog", "配网看门狗", "300 s", 300000,
     ("kconfig", "components/minihub_matter/Kconfig.minihub", "MINIHUB_MATTER_COMMISSION_WAIT_MS")),
    ("matter", "Subscription report interval", "订阅上报间隔", "5–30 s", 5,
     ("define", "components/minihub_matter/matter_ctrl.cpp", "MATTER_SUB_MIN_INTERVAL")),
    ("matter", "Re-arm backoff after a dropped subscription", "订阅掉线后重挂退避",
     "30 s – 10 min", 30000,
     ("define", "components/minihub_protocol_adapters/adapter_matter/matter_adapter.c",
      "SUB_ARM_MIN_MS")),

    ("radios", "MQTT devices per adapter", "单适配器 MQTT 设备数", "16", 16,
     ("define", "components/minihub_protocol_adapters/adapter_mqtt/mqtt_adapter.c",
      "MAX_MQTT_DEVICES")),
    ("radios", "Modbus gateways", "Modbus 网关数", "8", 8,
     ("define", "components/minihub_protocol_adapters/adapter_modbus/modbus_map.h",
      "MODBUS_MAX_GATEWAYS")),
    ("radios", "Modbus units per gateway", "每网关 Modbus 单元数", "16", 16,
     ("define", "components/minihub_protocol_adapters/adapter_modbus/modbus_map.h",
      "MODBUS_MAX_UNITS_PER_GW")),
    ("radios", "Zigbee per-step discovery timeout", "Zigbee 每步发现超时", "12 s", 12000,
     ("define", "components/minihub_zigbee/zigbee_stack.c", "DISCOVER_STEP_TIMEOUT_MS")),
    ("radios", "BLE Mesh provisioning watchdog", "BLE Mesh 配网看门狗", "45 s", 45000000,
     ("define", "components/minihub_ble_mesh/ble_mesh_stack.c", "PROV_BUSY_WATCHDOG_US")),

    ("update", "Firmware release note", "固件 release notes 体积", "8 KiB", 8192,
     ("define", "components/minihub_update/minihub_update.c", "MH_UPDATE_NOTE_MAX")),
    ("update", "Deploy index body", "部署索引体积", "256 KiB", 262144,
     ("define", "components/minihub_update/update_internal.h", "MH_UPDATE_INDEX_MAX")),
    ("update", "Update URL", "更新 URL 长度", "512", 512,
     ("define", "components/minihub_update/update_internal.h", "MH_UPDATE_URL_MAX")),

    ("js", "JS worker stack", "JS worker 栈", "32 KiB", 32768,
     ("kconfig", "components/minihub_js/Kconfig.minihub", "MINIHUB_JS_WORKER_STACK_SIZE")),
    ("js", "JS worker stack budget", "JS worker 栈预算", "24 KiB", 24576,
     ("kconfig", "components/minihub_js/Kconfig.minihub", "MINIHUB_JS_WORKER_STACK_BUDGET")),
    ("js", "HTTP body one JS request holds", "单个 JS 请求持有的 HTTP body", "64 KiB", 65536,
     ("define", "components/minihub_js/minihub_js.c", "MINIHUB_JS_HTTP_BODY_MAX")),

    ("hub", "Hub reload task stack", "Hub 重载任务栈", "24 KiB", 24576,
     ("kconfig", "components/minihub_hub/Kconfig.minihub", "MINIHUB_HUB_RELOAD_STACK_SIZE")),
]

GROUPS = ("core", "rules", "scenes", "store", "web", "auth", "matter", "radios",
          "update", "js", "hub")

_HEADINGS = {
    "core": ("Point registry and core buffers", "点位注册表与核心缓冲"),
    "rules": ("Rules engine", "规则引擎"),
    "scenes": ("Scenes", "场景"),
    "store": ("Store packages and modules", "商店包与模块"),
    "web": ("Web console request sizes", "Web 控制台请求体积"),
    "auth": ("Login and sessions", "登录与会话"),
    "matter": ("Matter controller", "Matter 控制器"),
    "radios": ("Radios and adapters", "射频与适配器"),
    "update": ("Firmware and asset update", "固件与资源更新"),
    "js": ("JavaScript runtime", "JavaScript 运行时"),
    "hub": ("Hub reload", "Hub 重载"),
}


_EXPR = re.compile(r"(\d+|[()+\-*/])")


def _num(text: str) -> int | None:
    """Evaluate a C constant expression: 1024, (128 * 1024), 2u * 1024u, 45 * 1000000ULL.

    A three-rule recursive-descent parser over digits, + - * / and parentheses, so
    nothing but arithmetic can be evaluated here.
    """
    cleaned = re.sub(r"([0-9])\s*(?:unsigned\s+)?[uUlL]+", r"\1", text)
    cleaned = re.sub(r"\bunsigned\s+", "", cleaned, flags=re.I)
    toks = _EXPR.findall(cleaned)
    if not toks or "".join(toks) != re.sub(r"\s", "", cleaned):
        return None
    pos = 0

    def factor() -> int | None:
        nonlocal pos
        if pos >= len(toks):
            return None
        tok = toks[pos]
        if tok.isdigit():
            pos += 1
            return int(tok)
        if tok != "(":
            return None
        pos += 1
        val = expr()
        if val is None or pos >= len(toks) or toks[pos] != ")":
            return None
        pos += 1
        return val

    def term() -> int | None:
        nonlocal pos
        val = factor()
        while val is not None and pos < len(toks) and toks[pos] in ("*", "/"):
            op = toks[pos]
            pos += 1
            rhs = factor()
            if rhs is None or (op == "/" and rhs == 0):
                return None
            val = val * rhs if op == "*" else val // rhs
        return val

    def expr() -> int | None:
        nonlocal pos
        val = term()
        while val is not None and pos < len(toks) and toks[pos] in ("+", "-"):
            op = toks[pos]
            pos += 1
            rhs = term()
            if rhs is None:
                return None
            val = val + rhs if op == "+" else val - rhs
        return val

    out = expr()
    return out if out is not None and pos == len(toks) else None


def _read(rel: str) -> list[str]:
    path = ROOT / rel
    if not path.is_file():
        raise ExtensionError(f"minihub-limits: source {rel} not found")
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def actual_value(spec) -> tuple[int | str | None, str]:
    """Return (value, citation) for one source spec, or (None, reason).

    The citation is resolved here rather than stored in the table. It names the
    file and, where available, a stable anchor (a ``#define``/Kconfig symbol or a
    captured function name) — never a line number, so the guide does not have to
    track source line positions.
    """
    kind = spec[0]
    if kind == "define":
        _, rel, symbol = spec
        pat = re.compile(rf"^#define\s+{re.escape(symbol)}\s+(\S.*)$")
        for i, line in enumerate(_read(rel), 1):
            m = pat.match(line.strip())
            if m:
                raw = m.group(1).split("/*")[0].strip()
                n = _num(raw)
                return (n if n is not None else raw.strip('"'), f"{rel} ({symbol})")
        return None, f"{rel}: no #define {symbol}"
    if kind == "kconfig":
        _, rel, symbol = spec
        lines = _read(rel)
        for i, line in enumerate(lines):
            if line.strip() == f"config {symbol}":
                for follow in lines[i + 1:i + 12]:
                    m = re.match(r"\s+default\s+(\S+)", follow)
                    if m:
                        n = _num(m.group(1))
                        return (n if n is not None else m.group(1), f"{rel} ({symbol})")
                return None, f"{rel}: config {symbol} has no default"
        return None, f"{rel}: no config {symbol}"
    if kind == "line":
        _, rel, pattern = spec
        text = "\n".join(_read(rel))
        m = re.search(pattern, text, re.DOTALL)
        if not m:
            return None, f"{rel}: no match for {pattern!r}"
        groups = m.groups()
        raw = groups[-1]
        val = _num(raw) if _num(raw) is not None else raw
        # An optional leading capture group names a stable anchor (function or
        # symbol); otherwise the file alone is cited. No line number is ever
        # emitted, so the guide does not couple to source line positions.
        name = groups[0] if len(groups) > 1 else None
        cite = f"{rel} ({name})" if name else rel
        return (val, cite)
    return None, f"unknown source kind {kind}"


def verify() -> list[str]:
    """Drift messages: every row must still match the code it cites."""
    problems = []
    for row in ROWS:
        want = row[4]
        got, where = actual_value(row[5])
        if got is None:
            problems.append(f"{row[1]}: {where}")
        elif want is not None and got != want:
            problems.append(f"{row[1]}: {where} says {got}, the guide says {want}")
    return problems


def rows_rst(group: str, lang: str) -> list[str]:
    """One captioned table per group. Every row cites where it is defined (file,
    plus a stable symbol/function name where one exists) with no line number, so
    the table does not couple to source line positions and only the value is
    enforced by verify()."""
    zh = lang.startswith("zh")
    en_head, zh_head = _HEADINGS[group]
    header = (["上限项", "数值", "定义位置"] if zh else ["Bound", "Value", "Defined at"])
    rows = []
    for row in ROWS:
        if row[0] != group:
            continue
        _got, cite = actual_value(row[5])
        rows.append([row[2] if zh else row[1], f"``{row[3]}``", f"``{cite}``"])
    if not rows:
        raise ExtensionError(f"minihub-limits: no rows in group {group!r}")
    out = _table(header, [40, 14, 46], rows)
    out[0] = f".. list-table:: {zh_head if zh else en_head}"
    return out


class LimitsTable(Directive):
    required_arguments = 1
    optional_arguments = 0
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        group = self.arguments[0].strip().lower()
        if group not in GROUPS:
            raise self.error(f"unknown limits group {group!r} (use one of {', '.join(GROUPS)})")
        problems = verify()
        if problems:
            raise ExtensionError("minihub-limits is out of date:\n  "
                                 + "\n  ".join(problems))
        lines = rows_rst(group, env.config.language)
        node = nodes.Element()
        self.state.nested_parse(StringList(lines), 0, node)
        return node.children


def setup(app):
    app.add_directive("minihub-limits", LimitsTable)
    return {"parallel_read_safe": True, "version": "1.0"}

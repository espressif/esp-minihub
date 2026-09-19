#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Wrap .. rubric:: blocks in host_rest / plugin_rest / mcp RST with styled containers."""

from __future__ import annotations

import re
import sys
from pathlib import Path

RUBRIC = re.compile(r"^\.\. rubric:: (.+)$")
SECTION = re.compile(r"^[A-Za-z0-9_./ ·]+$\n^\^+$", re.MULTILINE)
ENDPOINT = re.compile(r"^(GET|POST|PUT|DELETE|PATCH) (.+)$")
WS_ENDPOINT = re.compile(r"^(GET) (/ws/.+)$")
MCP_TOOL = re.compile(r"^[a-z][a-z0-9_.]+$")


def rubric_class(title: str) -> str:
    lower = title.lower()
    if "attention" in lower:
        return "mh-api-block mh-api-attention"
    if "return" in lower:
        return "mh-api-block mh-api-returns"
    if "parameter" in lower:
        return "mh-api-block mh-api-parameters"
    return "mh-api-block"


def is_endpoint_title(line: str, nxt: str | None) -> bool:
    if nxt is None or not re.fullmatch(r"\^+", nxt):
        return False
    if ENDPOINT.match(line) or WS_ENDPOINT.match(line):
        return True
    if MCP_TOOL.match(line) and "/" not in line:
        return True
    # Combined titles like "GET /api/rules · POST /api/rules/import"
    if " · " in line and ("/api/" in line or line.startswith("GET ") or line.startswith("POST ")):
        return True
    return False


def command_literal(title_line: str) -> str | None:
    if ENDPOINT.match(title_line) or WS_ENDPOINT.match(title_line):
        return f"``{title_line.strip()}``"
    if MCP_TOOL.match(title_line):
        return f"``{title_line.strip()}``"
    if " · " in title_line and "/api/" in title_line:
        parts = [p.strip() for p in title_line.split("·")]
        return "\n\n   ".join(f"``{p}``" for p in parts)
    return None


def indent_block(lines: list[str], spaces: int = 3) -> list[str]:
    prefix = " " * spaces
    out: list[str] = []
    for line in lines:
        if line.strip() == "":
            out.append("")
        else:
            out.append(prefix + line)
    return out


def next_section_start(lines: list[str], i: int) -> int:
    while i < len(lines):
        if RUBRIC.match(lines[i]):
            return i
        if i + 1 < len(lines) and is_endpoint_title(lines[i], lines[i + 1]):
            return i
        if lines[i].startswith(".. ") and not lines[i].startswith(".. rubric::"):
            # directive at section level (list-table, code-block, etc.)
            if i > 0 and lines[i - 1].strip() == "":
                prev = i - 2
                while prev >= 0 and lines[prev].strip() == "":
                    prev -= 1
                if prev >= 0 and RUBRIC.match(lines[prev]):
                    pass  # still inside attention block
                else:
                    return i
        i += 1
    return len(lines)


def collect_rubric_body(lines: list[str], start: int) -> tuple[list[str], int]:
    i = start
    while i < len(lines) and lines[i].strip() == "":
        i += 1
    body: list[str] = []
    end = next_section_start(lines, i)
    body = lines[i:end]
    while body and body[-1].strip() == "":
        body.pop()
    return body, end


def wrap_file(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if ".. container:: mh-api-block" in text:
        print(f"skip (already wrapped): {path}")
        return

    lines = text.splitlines()
    out: list[str] = []
    i = 0
    pending_command: str | None = None

    while i < len(lines):
        line = lines[i]
        nxt = lines[i + 1] if i + 1 < len(lines) else None

        if is_endpoint_title(line, nxt):
            out.append(line)
            out.append(nxt)
            i += 2
            pending_command = command_literal(line)
            while i < len(lines) and lines[i].strip() == "":
                out.append(lines[i])
                i += 1
            continue

        if pending_command and not RUBRIC.match(line):
            # Description lines before first rubric; insert command box first.
            cmd_lines = [
                ".. container:: mh-api-block mh-api-command",
                "",
                f"   {pending_command}",
                "",
            ]
            out.extend(cmd_lines)
            pending_command = None

        m = RUBRIC.match(line)
        if m:
            if pending_command:
                out.extend(
                    [
                        ".. container:: mh-api-block mh-api-command",
                        "",
                        f"   {pending_command}",
                        "",
                    ]
                )
                pending_command = None

            title = m.group(1)
            body, end = collect_rubric_body(lines, i + 1)
            cls = rubric_class(title)
            out.append(f".. container:: {cls}")
            out.append("")
            out.append(f"   **{title}**")
            out.append("")
            out.extend(indent_block(body))
            out.append("")
            i = end
            continue

        out.append(line)
        i += 1

    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrapped: {path}")


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parents[1]
    targets = argv[1:] or [
        "en/api/host_rest.rst",
        "zh_CN/api/host_rest.rst",
        "en/api/plugin_rest.rst",
        "zh_CN/api/plugin_rest.rst",
        "en/api/mcp.rst",
        "zh_CN/api/mcp.rst",
    ]
    for rel in targets:
        wrap_file(root / rel)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
